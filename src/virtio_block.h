/* Transitional VirtIO block: bounded DMA slots, IRQ completion and a blocked
 * caller. A transfer is split into up to VIRTIO_SLOTS descriptor chains that
 * are submitted together with one notification; the caller sleeps until the
 * used ring reports every chain, so a 512 KiB request costs one wake-up.
 * Filesystem serialization owns the queue; early boot uses bounded polling. */
static u16 virtio_port,virtio_queue_size,virtio_avail,virtio_used;
static u64 virtio_sectors;
static u32 virtio_features;
static int virtio_ready,virtio_present;
static int virtio_message_mode;
static u32 virtio_irq_line,virtio_slots;
static int virtio_waiter=-1;
static u64 virtio_deadline;
static u16 virtio_expected;
volatile u64 virtio_interrupts,virtio_suspensions,virtio_timeouts,virtio_requests,virtio_batches,virtio_batched_requests,virtio_max_batch;
static u32 io_read32(u16 port){u32 value;__asm__ volatile("inl %1,%0":"=a"(value):"Nd"(port));return value;}
static u16 io_read16(u16 port){u16 value;__asm__ volatile("inw %1,%0":"=a"(value):"Nd"(port));return value;}
static void io_write32(u16 port,u32 value){__asm__ volatile("outl %0,%1"::"a"(value),"Nd"(port));}
static u32 pci_read(u32 address,u32 reg){io_write32(0xcf8,0x80000000U|address|reg);return io_read32(0xcfc);}
#include "pci_irq.h"
typedef struct {u64 address;u32 length;u16 flags,next;} VirtioDescriptor;
/* 0x0d000000-0x0d0fffff: ring (16 KiB), request headers/status, then eight
 * 64 KiB bounce slots ending where the development-volume cache begins. */
#define VIRTIO_RING 0x0d000000ULL
#define VIRTIO_REQUESTS 0x0d004000ULL
#define VIRTIO_BOUNCE 0x0d080000ULL
#define VIRTIO_SLOTS 8
#define VIRTIO_SLOT_SECTORS 128
#define VIRTIO_MAX_SECTORS (VIRTIO_SLOTS*VIRTIO_SLOT_SECTORS)
static u64 virtio_request(u32 slot){return VIRTIO_REQUESTS+slot*32;}
static u8 *virtio_bounce(u32 slot){return (u8 *)(VIRTIO_BOUNCE+(u64)slot*VIRTIO_SLOT_SECTORS*512);}
static void virtio_block_init(void){
    for(u32 bus=0;bus<256;bus++)for(u32 slot=0;slot<32;slot++){
        u32 address=(bus<<16)|(slot<<11);if(pci_read(address,0)!=0x10011af4)continue;
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
        outb(virtio_port+18,7);virtio_ready=1;
        serial("VIRTIO: PCI block queue ready sectors=");hex(virtio_sectors);serial(" slots=");hex(virtio_slots);serial(virtio_message_mode==2?" MSI-X\r\n":virtio_message_mode?" MSI\r\n":" INTx\r\n");return;
    }
}
static void virtio_interrupt(u32 irq){
    if(!virtio_ready||(virtio_message_mode?irq!=48:irq!=virtio_irq_line))return;
    if(!virtio_message_mode){u8 status=inb(virtio_port+19);if(!(status&1))return;}virtio_interrupts++;
    /* Wake the caller only once every chain of its batch has been used. */
    volatile u16 *used=(volatile u16 *)((VIRTIO_RING+16*virtio_queue_size+6+2*virtio_queue_size+4095)&~4095ULL);
    if(virtio_waiter>=0&&(u16)(used[1]-virtio_used)>=virtio_expected){tasks[virtio_waiter].state=RUNNABLE;virtio_waiter=-1;}
}
static void virtio_timeout(void){
    if(virtio_waiter>=0&&timer_ticks>=virtio_deadline){
        outb(virtio_port+18,0);virtio_ready=0;virtio_timeouts++;
        tasks[virtio_waiter].state=RUNNABLE;virtio_waiter=-1;
    }
}
static int virtio_transfer(u64 sector,void *data,u32 count,int operation){
    if(!virtio_ready||count>virtio_slots*VIRTIO_SLOT_SECTORS||sector>virtio_sectors||count>virtio_sectors-sector)return 0;
    if(operation==4&&!(virtio_features&(1U<<9)))return 0;
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
    __asm__ volatile("mfence":::"memory");outw(virtio_port+16,0);
    virtio_requests++;virtio_batched_requests+=chains;if(chains>1)virtio_batches++;if(chains>virtio_max_batch)virtio_max_batch=chains;
    if(kernel_started&&(virtio_message_mode||(virtio_irq_line>0&&virtio_irq_line<16))){
        virtio_expected=chains;
        while((u16)(used[1]-virtio_used)<chains&&virtio_ready){virtio_waiter=current_task;virtio_deadline=timer_ticks+200+chains*50;
            tasks[current_task].state=WAIT_IO;virtio_suspensions++;kernel_suspend();}
        if(!virtio_ready)return 0;
    }else{
        u32 spins=100000000;while((u16)(used[1]-virtio_used)<chains&&--spins)__asm__ volatile("pause":::"memory");
        if(!spins){outb(virtio_port+18,0);virtio_ready=0;return 0;}
    }
    __asm__ volatile("mfence":::"memory");virtio_used=used[1];(void)inb(virtio_port+19);
    for(u32 i=0;i<chains;i++)if(*(volatile u8 *)(virtio_request(i)+16))return 0;
    if(operation==0)for(u32 i=0,done=0;i<chains;i++){u32 n=count-done;if(n>VIRTIO_SLOT_SECTORS)n=VIRTIO_SLOT_SECTORS;memcpy((u8 *)data+(u64)done*512,virtio_bounce(i),(u64)n*512);done+=n;}
    return 1;
}
