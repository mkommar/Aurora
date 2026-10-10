/* Transitional VirtIO-net and VirtIO-rng. DMA stays in reserved supervisor RAM.
 * Descriptor ownership changes use release/acquire barriers; lengths and IDs
 * supplied by the device are checked before accessing memory. */
#define NET_RX_RING 0x0e100000ULL
#define NET_TX_RING 0x0e110000ULL
#define NET_RX_DATA 0x0e120000ULL
#define NET_TX_DATA 0x0e1a0000ULL
#define RNG_RING 0x0e300000ULL
#define RNG_DATA 0x0e304000ULL
#define NET_BUFFER 2048
static u16 net_port,net_rx_size,net_tx_size,net_rx_used,net_tx_used,net_rx_avail,net_tx_avail;
static u16 entropy_port,entropy_size,entropy_avail,entropy_used;
static int net_ready,net_irq_mode,entropy_ready,net_announced;
static u32 net_device,entropy_device;
static u32 net_irq_line;
static int net_modern,entropy_modern;
static u64 net_common,net_notify_bar,net_device_config,entropy_common,entropy_notify;
static VirtioPciCapability net_notify_cap;
static u32 entropy_notify_multiplier;
static u8 net_tx_busy[256];
static u8 net_notify_announced[2];
static int net_tx_announced,net_rx_completion_announced;
#if defined(AURORA_NET_TRACE)
static u16 net_rx_reported_used;
static u8 net_packets_reported;
#endif
volatile u64 net_interrupts,net_bad_descriptors,net_entropy_bytes,net_entropy_failures;
static u64 ring_used(u64 base,u16 size){return (base+16*size+6+2*size+4095)&~4095ULL;}
static int virtio_modern_queue(u64 common,u16 index,u64 ring,u16 *size){
    virtio_mmio16_write(common,0x16,index);*size=virtio_mmio16(common,0x18);
    if(!virtio_pci_queue_valid(*size))return 0;
    memset((void *)ring,0,0x10000);virtio_mmio64_write(common,0x20,ring);
    virtio_mmio64_write(common,0x28,ring+16*(u64)*size);
    virtio_mmio64_write(common,0x30,ring+virtio_pci_queue_bytes(*size));
    virtio_mmio16_write(common,0x1c,1);return 1;
}
static void virtio_net_modern_notify(u64 common,u64 notify,u32 multiplier,u16 queue){
    u16 offset=virtio_mmio16(common,0x1e);
    *(volatile u16 *)(notify+(u64)offset*multiplier)=queue;
}
static int virtio_net_modern_capabilities(u32 device,VirtioPciCapability *common,
                                           VirtioPciCapability *notify,
                                           VirtioPciCapability *config);
static int virtio_modern_entropy_init(u32 address){
    VirtioPciCapability common={0},notify={0},config={0};
    if(pci_read(address,0)!=((u32)VIRTIO_PCI_DEVICE_RNG<<16|0x1af4))return 0;
    serial("RNG: modern PCI candidate\r\n");
    if(!virtio_net_modern_capabilities(address,&common,&notify,&config)){serial("RNG: modern capabilities rejected\r\n");return 0;}
    pci_write16(address,4,(pci_read(address,4)&0xffff)|5);
    u64 common_bar=virtio_bar(address,common.bar),notify_bar=virtio_bar(address,notify.bar);
    if(!common_bar||!notify_bar||!dma_assign_device(address,DMA_DOMAIN_ENTROPY)){serial("RNG: modern DMA setup rejected\r\n");return 0;}
    entropy_device=address;entropy_common=common_bar+common.offset;entropy_notify=notify_bar+notify.offset;
    entropy_notify_multiplier=notify.notify_multiplier;
    *(volatile u8 *)(entropy_common+0x14)=0;*(volatile u8 *)(entropy_common+0x14)=1;*(volatile u8 *)(entropy_common+0x14)=3;
    virtio_mmio32_write(entropy_common,0x00,0);u64 features=virtio_mmio32(entropy_common,0x04);
    virtio_mmio32_write(entropy_common,0x00,1);features|=(u64)virtio_mmio32(entropy_common,0x04)<<32;
    if(!virtio_pci_modern_features_valid(features,(1ULL<<VIRTIO_F_VERSION_1)|(1ULL<<VIRTIO_F_ACCESS_PLATFORM))){*(volatile u8 *)(entropy_common+0x14)=0;return 0;}
    virtio_mmio32_write(entropy_common,0x08,0);virtio_mmio32_write(entropy_common,0x0c,0);
    virtio_mmio32_write(entropy_common,0x08,1);virtio_mmio32_write(entropy_common,0x0c,1U<<1);
    *(volatile u8 *)(entropy_common+0x14)=0x0b;if(!(*(volatile u8 *)(entropy_common+0x14)&8)){*(volatile u8 *)(entropy_common+0x14)=0;return 0;}
    if(!virtio_modern_queue(entropy_common,0,RNG_RING,&entropy_size)){*(volatile u8 *)(entropy_common+0x14)=0;return 0;}
    *(volatile u8 *)(entropy_common+0x14)=0x0f;entropy_modern=1;entropy_ready=1;
    serial("RNG: modern VirtIO entropy queue ready\r\n");return 1;
}
static void entropy_init(void){
#if defined(AURORA_NET_TRACE)
    serial("RNG: scanning VirtIO PCI devices\r\n");
#endif
    for(u32 bus=0;bus<256;bus++)for(u32 slot=0;slot<32;slot++){
        if(virtio_modern_entropy_init((bus<<16)|(slot<<11)))return;
        u32 device=(bus<<16)|(slot<<11);if(pci_read(device,0)!=0x10051af4)continue;
#if defined(AURORA_NET_TRACE)
        serial("RNG: VirtIO PCI device found bus=");hex(bus);serial(" slot=");hex(slot);serial("\r\n");
#endif
        if(!dma_assign_device(device,DMA_DOMAIN_ENTROPY)){
#if defined(AURORA_NET_TRACE)
            serial("RNG: DMA device assignment rejected\r\n");
#endif
            continue;}entropy_device=device;
        u32 bar=pci_read(device,0x10);if(!(bar&1)||bar>65535)continue;
        entropy_port=bar&~3U;pci_write16(device,4,(pci_read(device,4)&0xffff)|5);
        outb(entropy_port+18,0);outb(entropy_port+18,1);outb(entropy_port+18,3);io_write32(entropy_port+4,0);
        outw(entropy_port+14,0);entropy_size=io_read16(entropy_port+12);
        if(!entropy_size||entropy_size>256){
#if defined(AURORA_NET_TRACE)
            serial("RNG: invalid queue size\r\n");
#endif
            outb(entropy_port+18,128);return;}
        memset((void *)RNG_RING,0,16384);*(u16 *)(RNG_RING+16*entropy_size)=1;
        io_write32(entropy_port+8,RNG_RING/4096);outb(entropy_port+18,7);entropy_ready=1;
        serial("RNG: VirtIO hardware entropy queue ready\r\n");return;
    }
}
static i64 entropy_fill(void *output,u64 count){
    if(!entropy_ready)return -19;if(!count)return 0;u64 completed=0;
    if(!dma_validate(entropy_device,DMA_DOMAIN_ENTROPY,RNG_RING,0x4000,DMA_READ|DMA_WRITE) ||
       !dma_validate(entropy_device,DMA_DOMAIN_ENTROPY,RNG_DATA,0x1000,DMA_WRITE)) return -19;
    while(completed<count){u32 n=count-completed>256?256:(u32)(count-completed);
        VirtioDescriptor *d=(void *)RNG_RING;d[0]=(VirtioDescriptor){RNG_DATA,n,2,0};
        volatile u16 *avail=(void *)(RNG_RING+16*entropy_size),*used=(void *)ring_used(RNG_RING,entropy_size);
        avail[2+entropy_avail%entropy_size]=0;__atomic_thread_fence(__ATOMIC_RELEASE);avail[1]=++entropy_avail;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);if(entropy_modern)virtio_net_modern_notify(entropy_common,entropy_notify,entropy_notify_multiplier,0);else outw(entropy_port+16,0);
        u32 spins=10000000;while(used[1]==entropy_used&&--spins)__asm__ volatile("pause":::"memory");
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        volatile u32 *element=(void *)((u64)used+4+8*(entropy_used%entropy_size));
        if(!spins||(u16)(used[1]-entropy_used)!=1||element[0]!=0||!element[1]||element[1]>n){
            entropy_ready=0;net_entropy_failures++;outb(entropy_port+18,0);return completed?(i64)completed:-5;}
        n=element[1];memcpy((u8 *)output+completed,(void *)RNG_DATA,n);memset((void *)RNG_DATA,0,n);
        completed+=n;entropy_used++;(void)inb(entropy_port+19);
    }
    net_entropy_bytes+=completed;return completed;
}
uint32_t aurora_net_random(void){u32 value=0;if(entropy_fill(&value,4)!=4){entropy_ready=0;net_ready=0;}return value;}
uint32_t aurora_net_now(void){return (u32)(timer_ticks*10);}
void aurora_net_panic(const char *message){serial(message);panic(" lwIP invariant\r\n");}
static void net_reclaim_tx(void){
    volatile u16 *used=(void *)ring_used(NET_TX_RING,net_tx_size);__atomic_thread_fence(__ATOMIC_ACQUIRE);
    if((u16)(used[1]-net_tx_used)>net_tx_size){net_bad_descriptors++;net_ready=0;return;}
    while(net_tx_used!=used[1]){volatile u32 *entry=(void *)((u64)used+4+8*(net_tx_used%net_tx_size));u32 id=entry[0];
        if(id>=net_tx_size||!net_tx_busy[id]){net_bad_descriptors++;net_ready=0;return;}
        net_tx_busy[id]=0;net_tx_used++;}
}
static void net_notify_queue(u16 queue){
    u64 address;
    if (net_modern) {
        virtio_mmio16_write(net_common,0x16,queue);
        address=virtio_pci_notify_address(&net_notify_cap,net_notify_bar,
                                          virtio_mmio16(net_common,0x1e));
        if (!address) { net_ready=0; return; }
        if (!net_notify_announced[queue]) { serial("NET: modern notify queue=");hex(queue);serial(" address=");hex(address);serial("\r\n");net_notify_announced[queue]=1; }
        *(volatile u16 *)address=0;
    } else outw(net_port+16,queue);
}
int aurora_net_transmit(const void *packet,unsigned length){
    if(!net_ready||!entropy_ready||length>1518||length<14 ||
       !dma_validate(net_device,DMA_DOMAIN_NET,NET_TX_RING,0x10000,DMA_READ|DMA_WRITE) ||
       !dma_validate(net_device,DMA_DOMAIN_NET,NET_TX_DATA,0x80000,DMA_READ|DMA_WRITE))return 0;
    net_reclaim_tx();if(!net_ready)return 0;
    u32 id;for(id=0;id<net_tx_size&&net_tx_busy[id];id++){}if(id==net_tx_size)return 0;
    u8 *buffer=(void *)(NET_TX_DATA+id*NET_BUFFER);memset(buffer,0,10);memcpy(buffer+10,packet,length);
    VirtioDescriptor *d=(void *)NET_TX_RING;d[id]=(VirtioDescriptor){(u64)buffer,length+10,0,0};net_tx_busy[id]=1;
    volatile u16 *avail=(void *)(NET_TX_RING+16*net_tx_size);avail[2+net_tx_avail%net_tx_size]=id;
    __atomic_thread_fence(__ATOMIC_RELEASE);avail[1]=++net_tx_avail;
    if (net_modern&&!net_tx_announced) { serial("NET: modern TX publication queue=1\r\n");net_tx_announced=1; }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);net_notify_queue(1);return net_ready;
}
static void net_poll(void){
    if(!net_ready)return;
    if(!dma_validate(net_device,DMA_DOMAIN_NET,NET_RX_RING,0x10000,DMA_READ|DMA_WRITE) ||
       !dma_validate(net_device,DMA_DOMAIN_NET,NET_RX_DATA,0x100000,DMA_READ|DMA_WRITE)) { net_ready=0; return; }
    net_reclaim_tx();if(!net_ready)return;
    volatile u16 *used=(void *)ring_used(NET_RX_RING,net_rx_size),*avail=(void *)(NET_RX_RING+16*net_rx_size);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
#if defined(AURORA_NET_TRACE)
    if(net_modern&&used[1]!=net_rx_reported_used){serial("NET: modern RX used=");hex(used[1]);serial(" avail=");hex(avail[1]);serial("\r\n");net_rx_reported_used=used[1];}
#endif
    if((u16)(used[1]-net_rx_used)>net_rx_size){net_bad_descriptors++;net_ready=0;return;}
    unsigned budget=64;int notify=0;
    while(net_rx_used!=used[1]&&budget--){volatile u32 *entry=(void *)((u64)used+4+8*(net_rx_used%net_rx_size));
        u32 id=entry[0],length=entry[1];
#if defined(AURORA_NET_TRACE)
        if(net_modern){serial("NET: modern RX used entry id=");hex(id);serial(" length=");hex(length);serial(" buffer=");hex(NET_RX_DATA+(u64)id*NET_BUFFER);serial("\r\n");}
#endif
        if(id>=net_rx_size){net_bad_descriptors++;net_ready=0;return;}
        u8 *buffer=(void *)(NET_RX_DATA+id*NET_BUFFER);
        if(length>=24&&length<=1528&&!buffer[0]&&!buffer[1]){
#if defined(AURORA_NET_TRACE)
            if(net_modern&&net_packets_reported<8){serial("NET: packet parsed length=");hex(length-10);serial(" ethertype=");hex(((u16)buffer[12]<<8)|buffer[13]);serial("\r\n");net_packets_reported++;}
#endif
            network_input(buffer+10,length-10);
        }else net_bad_descriptors++;
        if (net_modern&&!net_rx_completion_announced) { serial("NET: modern RX completion queue=0\r\n");net_rx_completion_announced=1; }
        net_rx_used++;avail[2+net_rx_avail%net_rx_size]=id;net_rx_avail++;notify=1;
    }
    if(notify){__atomic_thread_fence(__ATOMIC_RELEASE);avail[1]=net_rx_avail;__atomic_thread_fence(__ATOMIC_SEQ_CST);net_notify_queue(0);}
    network_tick();if(!net_announced&&network_configured()){net_announced=1;serial("NET: DHCP lease acquired\r\n");serial("NET: IPv4 readiness confirmed\r\n");serial("NET: DHCP IPv4 address, gateway and TCP/UDP ready\r\n");}
}
static void net_interrupt(u32 irq){
    if(!net_ready||(net_irq_mode?irq!=49:irq!=net_irq_line))return;
    if(!net_irq_mode&&!(inb(net_port+19)&3))return;net_interrupts++;
}
static int virtio_net_modern_capabilities(u32 device,VirtioPciCapability *common,
                                           VirtioPciCapability *notify,
                                           VirtioPciCapability *config){
    u8 cap=pci_config8(device,0x34);int found=0;
    for(int hops=0;cap>=0x40&&cap<=0xfc&&hops<48;hops++){
        u32 value=pci_read(device,cap);u8 id=value&255,next=(value>>8)&0xff;
        if(id==9){
            u32 header=pci_read(device,cap+4);VirtioPciCapability *out=0;
            u8 type=(value>>24)&0xff;
            if(type==VIRTIO_PCI_CAP_COMMON)out=common;
            if(type==VIRTIO_PCI_CAP_NOTIFY)out=notify;
            if(type==VIRTIO_PCI_CAP_DEVICE)out=config;
            if(out){out->type=type;out->bar=header&0xff;out->offset=pci_read(device,cap+8);
                out->length=pci_read(device,cap+12);out->notify_multiplier=0;
                if(type==VIRTIO_PCI_CAP_NOTIFY)out->notify_multiplier=pci_read(device,cap+16);
                found++;}
        }
        if(next==cap)break;cap=next;
    }
    return found>=3&&virtio_pci_capabilities_complete(common,notify,config);
}
static int virtio_net_modern_init(u32 device){
    VirtioPciCapability common={0},notify={0},config={0};
    if(!virtio_net_modern_capabilities(device,&common,&notify,&config))return 0;
    u64 common_bar=virtio_bar(device,common.bar),notify_bar=virtio_bar(device,notify.bar),config_bar=virtio_bar(device,config.bar);
    if(!common_bar||!notify_bar||!config_bar)return 0;
    if(!dma_assign_device(device,DMA_DOMAIN_NET))return 0;net_device=device;
#if defined(AURORA_NET_TRACE)
    serial("NET: modern DMA device assigned\r\n");
#endif
    pci_write16(device,4,(pci_read(device,4)&0xffff)|5);
    net_common=common_bar+common.offset;net_notify_bar=notify_bar;net_device_config=config_bar+config.offset;
    net_notify_cap=notify;net_modern=1;net_port=0;net_irq_mode=0;net_irq_line=0;
    *(volatile u8 *)(net_common+0x14)=0;*(volatile u8 *)(net_common+0x14)=1;*(volatile u8 *)(net_common+0x14)=3;
    u64 features=virtio_mmio32(net_common,0x04);virtio_mmio32_write(net_common,0x00,1);features|=(u64)virtio_mmio32(net_common,0x04)<<32;
    if(!(features&(1ULL<<32))||!(features&(1ULL<<33))||!(features&(1ULL<<5))){*(volatile u8 *)(net_common+0x14)=0;return 0;}
    virtio_mmio32_write(net_common,0x08,0);virtio_mmio32_write(net_common,0x0c,(1U<<5));
    virtio_mmio32_write(net_common,0x08,1);virtio_mmio32_write(net_common,0x0c,3U);
    *(volatile u8 *)(net_common+0x14)=0x0b;
    if(!(*(volatile u8 *)(net_common+0x14)&8)){*(volatile u8 *)(net_common+0x14)=0;return 0;}
    virtio_mmio16_write(net_common,0x16,0);net_rx_size=virtio_mmio16(net_common,0x18);
    virtio_mmio16_write(net_common,0x16,1);net_tx_size=virtio_mmio16(net_common,0x18);
    if(!net_rx_size||net_rx_size>256||!net_tx_size||net_tx_size>256){*(volatile u8 *)(net_common+0x14)=0;return 0;}
    memset((void *)NET_RX_RING,0,16384);memset((void *)NET_TX_RING,0,16384);
    virtio_mmio16_write(net_common,0x16,0);virtio_mmio64_write(net_common,0x20,NET_RX_RING);virtio_mmio64_write(net_common,0x28,NET_RX_RING+16*net_rx_size);virtio_mmio64_write(net_common,0x30,ring_used(NET_RX_RING,net_rx_size));virtio_mmio16_write(net_common,0x1c,1);
    virtio_mmio16_write(net_common,0x16,1);virtio_mmio64_write(net_common,0x20,NET_TX_RING);virtio_mmio64_write(net_common,0x28,NET_TX_RING+16*net_tx_size);virtio_mmio64_write(net_common,0x30,ring_used(NET_TX_RING,net_tx_size));virtio_mmio16_write(net_common,0x1c,1);
    u8 mac[6];for(int i=0;i<6;i++)mac[i]=*(volatile u8 *)(net_device_config+i);
    VirtioDescriptor *d=(void *)NET_RX_RING;volatile u16 *avail=(void *)(NET_RX_RING+16*net_rx_size);
    for(u16 i=0;i<net_rx_size;i++){d[i]=(VirtioDescriptor){NET_RX_DATA+i*NET_BUFFER,NET_BUFFER,2,0};avail[2+i]=i;}
    net_rx_avail=net_rx_size;__atomic_thread_fence(__ATOMIC_RELEASE);avail[1]=net_rx_avail;*(volatile u8 *)(net_common+0x14)=0x0f;net_ready=1;net_notify_queue(0);network_init(mac);
#if defined(AURORA_NET_TRACE)
    serial("NET: modern RX descriptors published queue=0 size=");hex(net_rx_size);serial(" avail=");hex(net_rx_avail);serial(" ring=");hex(NET_RX_RING);serial(" data=");hex(NET_RX_DATA);serial("\r\n");
#endif
    if(!dma_validate(net_device,DMA_DOMAIN_NET,NET_RX_RING,0x10000,DMA_READ|DMA_WRITE)||!dma_validate(net_device,DMA_DOMAIN_NET,NET_RX_DATA,0x100000,DMA_READ|DMA_WRITE)){
#if defined(AURORA_NET_TRACE)
        serial("NET: modern RX DMA mapping rejected\r\n");
#endif
        *(volatile u8 *)(net_common+0x14)=0;return 0;
    }
    serial("NET: modern VirtIO-net queue ready\r\n");return 1;
}
static void virtio_net_init(void){
    if(!entropy_ready)return;
    for(u32 bus=0;bus<256;bus++)for(u32 slot=0;slot<32;slot++){
        u32 modern=(bus<<16)|(slot<<11);if(pci_read(modern,0)==0x10411af4&&virtio_net_modern_init(modern))return;
    }
    for(u32 bus=0;bus<256;bus++)for(u32 slot=0;slot<32;slot++){
        u32 device=(bus<<16)|(slot<<11);if(pci_read(device,0)!=0x10001af4)continue;
        if(!dma_assign_device(device,DMA_DOMAIN_NET))continue;net_device=device;
        u32 bar=pci_read(device,0x10);if(!(bar&1)||bar>65535)continue;net_port=bar&~3U;
        pci_write16(device,4,(pci_read(device,4)&0xffff)|5);outb(net_port+18,0);outb(net_port+18,1);outb(net_port+18,3);
        u32 features=io_read32(net_port);if(!(features&(1U<<5))){outb(net_port+18,128);return;}
        io_write32(net_port+4,1U<<5);outw(net_port+14,0);net_rx_size=io_read16(net_port+12);
        outw(net_port+14,1);net_tx_size=io_read16(net_port+12);
        if(!net_rx_size||net_rx_size>256||!net_tx_size||net_tx_size>256){outb(net_port+18,128);return;}
        memset((void *)NET_RX_RING,0,16384);memset((void *)NET_TX_RING,0,16384);
        outw(net_port+14,0);io_write32(net_port+8,NET_RX_RING/4096);
        outw(net_port+14,1);io_write32(net_port+8,NET_TX_RING/4096);
        net_irq_line=pci_read(device,0x3c)&255;net_irq_mode=pci_message_irq(device,49);
        if(net_irq_mode==2){outw(net_port+20,0xffff);for(u16 queue=0;queue<2;queue++){outw(net_port+14,queue);outw(net_port+22,0);
            if(io_read16(net_port+22)==0xffff){outb(net_port+18,128);return;}}}
        u32 config=net_irq_mode==2?24:20;u8 mac[6];for(int i=0;i<6;i++)mac[i]=inb(net_port+config+i);
        VirtioDescriptor *d=(void *)NET_RX_RING;volatile u16 *avail=(void *)(NET_RX_RING+16*net_rx_size);
        for(u16 i=0;i<net_rx_size;i++){d[i]=(VirtioDescriptor){NET_RX_DATA+i*NET_BUFFER,NET_BUFFER,2,0};avail[2+i]=i;}
        net_rx_avail=net_rx_size;__atomic_thread_fence(__ATOMIC_RELEASE);avail[1]=net_rx_avail;
        outb(net_port+18,7);net_ready=1;outw(net_port+16,0);network_init(mac);
        serial(net_irq_mode==2?"NET: VirtIO-net MSI-X\r\n":net_irq_mode?"NET: VirtIO-net MSI\r\n":"NET: VirtIO-net INTx\r\n");return;
    }
}
