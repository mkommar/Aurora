/* Transitional VirtIO block: bounded DMA slots, IRQ completion and a blocked
 * caller. A transfer is split into up to VIRTIO_SLOTS descriptor chains that
 * are submitted together with one notification; the caller sleeps until the
 * used ring reports every chain, so a 512 KiB request costs one wake-up.
 * Filesystem serialization owns the queue; early boot uses bounded polling. */
#include "storage_service.h"
#include "virtio_pci.h"
static u16 virtio_port,virtio_queue_size,virtio_avail,virtio_used;
static u64 virtio_sectors;
static u32 virtio_features;
static u32 virtio_device;
static int virtio_ready,virtio_present;
static int virtio_message_mode;
static StorageIpcBroker virtio_storage_broker;
static StorageIpcService virtio_storage_service;
static u32 virtio_storage_sequence;
static u32 virtio_irq_line,virtio_slots;
#define VIRTIO_RING 0x0d000000ULL
#define VIRTIO_REQUESTS 0x0d004000ULL
#define VIRTIO_BOUNCE 0x0d080000ULL
#define VIRTIO_SLOTS 8
#define VIRTIO_SLOT_SECTORS 128
#define VIRTIO_MAX_SECTORS (VIRTIO_SLOTS*VIRTIO_SLOT_SECTORS)
static int virtio_modern;
static u64 virtio_common,virtio_notify,virtio_device_config;
static VirtioPciCapability virtio_notify_cap;
static u32 virtio_notify_multiplier;
static int virtio_storage_dma_check(u64 address,u64 length,u32 permissions){
    return dma_validate(virtio_device,DMA_DOMAIN_STORAGE,address,length,permissions);
}
static int virtio_waiter=-1;
static u64 virtio_deadline;
static u16 virtio_expected;
volatile u64 virtio_interrupts,virtio_suspensions,virtio_timeouts,virtio_requests,virtio_batches,virtio_batched_requests,virtio_max_batch;
static u32 io_read32(u16 port){u32 value;__asm__ volatile("inl %1,%0":"=a"(value):"Nd"(port));return value;}
static u16 io_read16(u16 port){u16 value;__asm__ volatile("inw %1,%0":"=a"(value):"Nd"(port));return value;}
static void io_write32(u16 port,u32 value){__asm__ volatile("outl %0,%1"::"a"(value),"Nd"(port));}
static u32 pci_read(u32 address,u32 reg){io_write32(0xcf8,0x80000000U|address|reg);return io_read32(0xcfc);}
#include "pci_irq.h"
static u8 pci_config8(u32 address,u32 reg){return (u8)(pci_read(address,reg&~3U)>>((reg&3)*8));}
static u64 virtio_bar(u32 address,u8 bar){
    u32 low=pci_read(address,0x10+bar*4);
    if(low&1)return 0;
    u64 base=low&~15U;
    if((low&6)==4){if(bar==5)return 0;base|=(u64)pci_read(address,0x14+bar*4)<<32;}
    return base;
}
static u32 virtio_mmio32(u64 address,u32 offset){return *(volatile u32 *)(address+offset);}
static u16 virtio_mmio16(u64 address,u32 offset){return *(volatile u16 *)(address+offset);}
static void virtio_mmio32_write(u64 address,u32 offset,u32 value){*(volatile u32 *)(address+offset)=value;}
static void virtio_mmio16_write(u64 address,u32 offset,u16 value){*(volatile u16 *)(address+offset)=value;}
static void virtio_mmio64_write(u64 address,u32 offset,u64 value){*(volatile u64 *)(address+offset)=value;}
static void virtio_modern_notify(void){
    u16 offset=virtio_mmio16(virtio_common,0x1e);
    u64 address=virtio_pci_notify_address(&virtio_notify_cap,virtio_notify,offset);
    if (!address) { virtio_ready=0; return; }
    *(volatile u16 *)address=0;
}
static int virtio_modern_capabilities(u32 device,VirtioPciCapability *common,
                                      VirtioPciCapability *notify,VirtioPciCapability *config){
    u8 cap=pci_config8(device,0x34);int found=0;
    for(int hops=0;cap>=0x40&&cap<=0xfc&&hops<48;hops++){
        u32 value=pci_read(device,cap);u8 id=value&255,next=(value>>8)&0xff;
        if(id==9){
            u32 header=pci_read(device,cap+4);VirtioPciCapability *out=0;u8 type=(value>>24)&0xff;
            if(type==VIRTIO_PCI_CAP_COMMON)out=common;
            if(type==VIRTIO_PCI_CAP_NOTIFY)out=notify;
            if(type==VIRTIO_PCI_CAP_DEVICE)out=config;
            if(out){out->type=type;out->bar=header&0xff;out->offset=pci_read(device,cap+8);out->length=pci_read(device,cap+12);out->notify_multiplier=0;
                if(out->type==VIRTIO_PCI_CAP_NOTIFY)out->notify_multiplier=pci_read(device,cap+16);found++;}
        }
        if(next==cap)break;cap=next;
    }
    return found>=3 && virtio_pci_capabilities_complete(common,notify,config);
}
static int virtio_modern_block_init(u32 address){
    VirtioPciCapability common={0},notify={0},config={0};
    if(pci_read(address,0)!=((u32)VIRTIO_PCI_DEVICE_BLOCK<<16|0x1af4) || !virtio_modern_capabilities(address,&common,&notify,&config))return 0;
    pci_write16(address,4,(pci_read(address,4)&0xffff)|5);
    u64 common_bar=virtio_bar(address,common.bar),notify_bar=virtio_bar(address,notify.bar),config_bar=virtio_bar(address,config.bar);
    if(!common_bar||!notify_bar||!config_bar)return 0;
    if(!dma_assign_device(address,DMA_DOMAIN_STORAGE))return 0;
    virtio_device=address;virtio_common=common_bar+common.offset;virtio_notify=notify_bar;
    virtio_device_config=config_bar+config.offset;virtio_notify_multiplier=notify.notify_multiplier;
    virtio_notify_cap=notify;
    *(volatile u8 *)(virtio_common+0x14)=0;*(volatile u8 *)(virtio_common+0x14)=1;*(volatile u8 *)(virtio_common+0x14)=3;
    u64 device_features=(u64)virtio_mmio32(virtio_common,0x04);
    virtio_mmio32_write(virtio_common,0x00,1);device_features|=(u64)virtio_mmio32(virtio_common,0x04)<<32;
    if(!(device_features&(1ULL<<VIRTIO_F_VERSION_1))){*(volatile u8 *)(virtio_common+0x14)=0;return 0;}
    /* ACCESS_PLATFORM is mandatory when QEMU routes this device through an
     * IOMMU; keep the negotiated set otherwise limited to split-ring support. */
    u64 driver_features=(1ULL<<VIRTIO_F_VERSION_1)|(1ULL<<VIRTIO_F_ACCESS_PLATFORM)|(device_features&(1ULL<<9));
    virtio_mmio32_write(virtio_common,0x08,0);virtio_mmio32_write(virtio_common,0x0c,(u32)driver_features);
    virtio_mmio32_write(virtio_common,0x08,1);virtio_mmio32_write(virtio_common,0x0c,(u32)(driver_features>>32));
    *(volatile u8 *)(virtio_common+0x14)=0x0b;
    if(!(*(volatile u8 *)(virtio_common+0x14)&0x08)){*(volatile u8 *)(virtio_common+0x14)=0;return 0;}
    virtio_mmio16_write(virtio_common,0x16,0);virtio_queue_size=virtio_mmio16(virtio_common,0x18);
    if(!virtio_pci_queue_valid(virtio_queue_size)){*(volatile u8 *)(virtio_common+0x14)=0;return 0;}
    virtio_slots=virtio_queue_size/3;if(virtio_slots>VIRTIO_SLOTS)virtio_slots=VIRTIO_SLOTS;
    memset((void *)VIRTIO_RING,0,16384);virtio_mmio64_write(virtio_common,0x20,VIRTIO_RING);
    virtio_mmio64_write(virtio_common,0x28,VIRTIO_RING+16*virtio_queue_size);
    virtio_mmio64_write(virtio_common,0x30,VIRTIO_RING+virtio_pci_queue_bytes(virtio_queue_size));
    virtio_mmio16_write(virtio_common,0x1c,1);virtio_modern=1;virtio_present=1;virtio_message_mode=0;virtio_irq_line=0;
    virtio_features=(u32)device_features;virtio_sectors=*(volatile u64 *)virtio_device_config;
    *(volatile u8 *)(virtio_common+0x14)=0x0f;
    storage_ipc_broker_init(&virtio_storage_broker,STORAGE_TASK,virtio_device,DMA_DOMAIN_STORAGE,virtio_sectors,SERVICE_CAP_STORAGE);
    storage_ipc_service_start(&virtio_storage_service,&virtio_storage_broker,1,STORAGE_TASK,SERVICE_CAP_STORAGE);virtio_ready=1;
    serial("VIRTIO: modern PCI block queue ready sectors=");hex(virtio_sectors);serial("\r\n");return 1;
}
typedef struct {u64 address;u32 length;u16 flags,next;} VirtioDescriptor;
/* 0x0d000000-0x0d0fffff: ring (16 KiB), request headers/status, then eight
 * 64 KiB bounce slots ending where the development-volume cache begins. */
static u64 virtio_request(u32 slot){return VIRTIO_REQUESTS+slot*32;}
static u8 *virtio_bounce(u32 slot){return (u8 *)(VIRTIO_BOUNCE+(u64)slot*VIRTIO_SLOT_SECTORS*512);}
static void virtio_block_init(void){
    for(u32 bus=0;bus<256;bus++)for(u32 slot=0;slot<32;slot++){
        u32 address=(bus<<16)|(slot<<11);if(virtio_modern_block_init(address))return;
    }
    for(u32 bus=0;bus<256;bus++)for(u32 slot=0;slot<32;slot++){
        u32 address=(bus<<16)|(slot<<11);if(pci_read(address,0)!=0x10011af4)continue;
        if(!dma_assign_device(address,DMA_DOMAIN_STORAGE))continue;virtio_device=address;
        u32 bar=pci_read(address,0x10);if(!(bar&1)||bar>65535)continue;
        virtio_present=1;virtio_port=bar&~3U;io_write32(0xcf8,0x80000000U|address|4);io_write32(0xcfc,(pci_read(address,4)&0xffff)|5);
        outb(virtio_port+18,0);outb(virtio_port+18,1);outb(virtio_port+18,3);
        virtio_features=io_read32(virtio_port)&(1U<<9);io_write32(virtio_port+4,virtio_features);
        outw(virtio_port+14,0);virtio_queue_size=io_read16(virtio_port+12);
        if(virtio_queue_size<3||virtio_queue_size>256){outb(virtio_port+18,128);return;}
        virtio_slots=virtio_queue_size/3;if(virtio_slots>VIRTIO_SLOTS)virtio_slots=VIRTIO_SLOTS;
        memset((void *)VIRTIO_RING,0,16384);virtio_irq_line=pci_read(address,0x3c)&255;
        io_write32(virtio_port+8,VIRTIO_RING/4096);
        virtio_message_mode=pci_message_irq(address,48);
        if(virtio_message_mode==2){outw(virtio_port+20,0xffff);outw(virtio_port+22,0);
            if(io_read16(virtio_port+22)==0xffff){pci_write16(address,pci_msix_cap+2,(pci_read(address,pci_msix_cap)>>16)&~0x8000);pci_write16(address,4,(pci_read(address,4)&0xffff)&~0x400);virtio_message_mode=0;}}
        u32 config=virtio_message_mode==2?24:20;
        virtio_sectors=io_read32(virtio_port+config)|((u64)io_read32(virtio_port+config+4)<<32);
         outb(virtio_port+18,7);storage_ipc_broker_init(&virtio_storage_broker,STORAGE_TASK,virtio_device,DMA_DOMAIN_STORAGE,virtio_sectors,SERVICE_CAP_STORAGE);storage_ipc_service_start(&virtio_storage_service,&virtio_storage_broker,1,STORAGE_TASK,SERVICE_CAP_STORAGE);virtio_ready=1;
        serial("VIRTIO: PCI block queue ready sectors=");hex(virtio_sectors);serial(" slots=");hex(virtio_slots);serial(virtio_message_mode==2?" MSI-X\r\n":virtio_message_mode?" MSI\r\n":" INTx\r\n");return;
    }
}
#if defined(AURORA_SELF_TEST) && defined(AURORA_DMA_FAULT_TEST)
/* Submit one genuine VirtIO request whose data address is outside the storage
 * second-level domain. This function is absent from production builds. */
static volatile int virtio_dma_test_pending;
static volatile u32 virtio_dma_test_polls;
static volatile u16 *virtio_dma_test_used;
static volatile u16 virtio_dma_test_used_before;
static int virtio_malicious_dma_test(void) {
    VirtioDescriptor *desc; volatile u16 *avail;
    u64 request = VIRTIO_REQUESTS + 0x20;
    /* This is ordinary guest RAM, but belongs to no VirtIO DMA domain. */
    const u64 unauthorized = 0x06000000ULL;
    if (!virtio_ready || !virtio_queue_size || dma_faulted()) return 0;
    desc = (VirtioDescriptor *)VIRTIO_RING;
    avail = (volatile u16 *)(VIRTIO_RING + 16 * virtio_queue_size);
    virtio_dma_test_used = (volatile u16 *)((VIRTIO_RING + 16 * virtio_queue_size + 6 + 2 * virtio_queue_size + 4095) & ~4095ULL);
    virtio_dma_test_used_before = virtio_dma_test_used[1];
    *(u32 *)request = 0; *(u32 *)(request + 4) = 0; *(u64 *)(request + 8) = 0;
    *(volatile u8 *)(request + 16) = 255;
    desc[0] = (VirtioDescriptor){request, 16, 1, 1};
    desc[1] = (VirtioDescriptor){unauthorized, 512, 3, 2};
    desc[2] = (VirtioDescriptor){request + 16, 1, 2, 0};
    avail[2 + virtio_avail % virtio_queue_size] = 0;
    __asm__ volatile("mfence" ::: "memory"); ++virtio_avail; avail[1] = virtio_avail;
    serial("IOMMU TEST: published descriptor=00000000 avail="); hex(virtio_avail);
    serial(" notify=00000000\r\n");
    __asm__ volatile("mfence" ::: "memory"); if(virtio_modern)virtio_modern_notify();else outw(virtio_port + 16, 0);
    (void)unauthorized;
    virtio_dma_test_pending = 1;
    return 1;
}
static void virtio_malicious_dma_test_poll(void) {
    if (!virtio_dma_test_pending) return;
    dma_fault_poll();
    if (dma_faulted()) {
        virtio_dma_test_pending = 0; virtio_ready = 0; outb(virtio_port + 18, 0);
        serial("IOMMU TEST: unauthorized VirtIO DMA blocked; fault latched; device quarantined source=");
        hex(dma_fault_source_id()); serial(" address="); hex(dma_fault_address_value()); serial("\r\n");
        if (storage_ipc_complete(&virtio_storage_broker, virtio_storage_sequence,
                                 STORAGE_IPC_OK, 0, 0) == STORAGE_IPC_E_SEQUENCE)
            serial("IOMMU TEST: stale completion rejected after quarantine\r\n");
        return;
    }
    if ((u16)(virtio_dma_test_used[1] - virtio_dma_test_used_before)) {
        serial("IOMMU TEST: device completed unauthorized chain\r\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    if (++virtio_dma_test_polls == 1000000) {
        serial("IOMMU TEST: no hardware fault was latched\r\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
}
#endif

#include "virtio_completion.h"
static int virtio_transfer(u64 sector,void *data,u32 count,int operation){
    if(!virtio_ready||count>virtio_slots*VIRTIO_SLOT_SECTORS||sector>virtio_sectors||count>virtio_sectors-sector)return 0;
    if(operation!=0&&operation!=1&&operation!=4)return 0;
    if(operation==4&&virtio_modern&&!(virtio_features&(1U<<9)))return 1;
    if(!dma_validate(virtio_device,DMA_DOMAIN_STORAGE,VIRTIO_RING,0x4000,DMA_READ|DMA_WRITE) ||
       !dma_validate(virtio_device,DMA_DOMAIN_STORAGE,VIRTIO_REQUESTS,0x1000,DMA_READ|DMA_WRITE) ||
       !dma_validate(virtio_device,DMA_DOMAIN_STORAGE,VIRTIO_BOUNCE,0x80000,DMA_READ|DMA_WRITE)) return 0;
    if(operation==4&&!(virtio_features&(1U<<9)))return 0;
    if(++virtio_storage_sequence==0)++virtio_storage_sequence;
    StorageIpcRequest ipc={STORAGE_IPC_VERSION,sizeof(StorageIpcRequest),operation==4?STORAGE_IPC_FLUSH:(operation==0?STORAGE_IPC_READ:STORAGE_IPC_WRITE),0,virtio_storage_sequence,STORAGE_TASK,virtio_device,DMA_DOMAIN_STORAGE,sector,count,count?(u64)VIRTIO_BOUNCE:0};
    if(storage_ipc_submit(&virtio_storage_broker,&ipc,SERVICE_CAP_STORAGE,
                          virtio_storage_dma_check)) return 0;
    VirtioDescriptor *desc=(VirtioDescriptor *)VIRTIO_RING;
    volatile u16 *avail=(volatile u16 *)(VIRTIO_RING+16*virtio_queue_size);
    volatile u16 *used=(volatile u16 *)((VIRTIO_RING+16*virtio_queue_size+6+2*virtio_queue_size+4095)&~4095ULL);
    u32 chains=0;
    for(u32 done=0;done<count||(operation==4&&!chains);chains++){
        u32 n=count-done;if(n>VIRTIO_SLOT_SECTORS)n=VIRTIO_SLOT_SECTORS;
        u64 request=virtio_request(chains);u8 *bounce=virtio_bounce(chains);
        if(operation==1)memcpy(bounce,(u8 *)data+(u64)done*512,(u64)n*512);
        *(u32 *)request=operation;*(u32 *)(request+4)=0;*(u64 *)(request+8)=sector+done;*(volatile u8 *)(request+16)=255;
        u16 head=chains*3;
        desc[head]=(VirtioDescriptor){request,16,1,operation==4?(u16)(head+2):(u16)(head+1)};
        desc[head+1]=(VirtioDescriptor){(u64)bounce,n*512,operation==0?3:1,(u16)(head+2)};
        desc[head+2]=(VirtioDescriptor){request+16,1,2,0};
        avail[2+(virtio_avail+chains)%virtio_queue_size]=head;done+=n;
    }
    __asm__ volatile("mfence":::"memory");virtio_avail+=chains;avail[1]=virtio_avail;
    __asm__ volatile("mfence":::"memory");if(virtio_modern)virtio_modern_notify();else outw(virtio_port+16,0);
    virtio_requests++;virtio_batched_requests+=chains;if(chains>1)virtio_batches++;if(chains>virtio_max_batch)virtio_max_batch=chains;
    if(kernel_started&&(virtio_message_mode||(virtio_irq_line>0&&virtio_irq_line<16))){
        virtio_expected=chains;
        /* Host-backed disks can stall during flushes and concurrent image I/O.
         * Keep a bounded watchdog, but allow 30 seconds at the 100 Hz PIT. */
        virtio_deadline=timer_ticks+3000+chains*50;
        while((u16)(used[1]-virtio_used)<chains&&virtio_ready){virtio_waiter=current_task;
            tasks[current_task].state=WAIT_IO;virtio_suspensions++;kernel_suspend();}
        if(!virtio_ready){storage_ipc_complete(&virtio_storage_broker,ipc.sequence,STORAGE_IPC_E_IO,0,0);return 0;}
    }else{
        u32 spins=100000000;while((u16)(used[1]-virtio_used)<chains&&--spins)__asm__ volatile("pause":::"memory");
        if(!spins){outb(virtio_port+18,0);virtio_ready=0;storage_ipc_complete(&virtio_storage_broker,ipc.sequence,STORAGE_IPC_E_IO,0,0);return 0;}
    }
    __asm__ volatile("mfence":::"memory");virtio_used=used[1];(void)inb(virtio_port+19);
    for(u32 i=0;i<chains;i++)if(*(volatile u8 *)(virtio_request(i)+16)){storage_ipc_complete(&virtio_storage_broker,ipc.sequence,STORAGE_IPC_E_IO,0,0);return 0;}
    if(operation==0)for(u32 i=0,done=0;i<chains;i++){u32 n=count-done;if(n>VIRTIO_SLOT_SECTORS)n=VIRTIO_SLOT_SECTORS;memcpy((u8 *)data+(u64)done*512,virtio_bounce(i),(u64)n*512);done+=n;}
    storage_ipc_complete(&virtio_storage_broker,ipc.sequence,STORAGE_IPC_OK,(u64)count*512,0);
    return 1;
}
