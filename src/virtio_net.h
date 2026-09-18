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
static u32 net_irq_line;
static u8 net_tx_busy[256];
volatile u64 net_interrupts,net_bad_descriptors,net_entropy_bytes,net_entropy_failures;
static u64 ring_used(u64 base,u16 size){return (base+16*size+6+2*size+4095)&~4095ULL;}
static void entropy_init(void){
    for(u32 bus=0;bus<256;bus++)for(u32 slot=0;slot<32;slot++){
        u32 device=(bus<<16)|(slot<<11);if(pci_read(device,0)!=0x10051af4)continue;
        u32 bar=pci_read(device,0x10);if(!(bar&1)||bar>65535)continue;
        entropy_port=bar&~3U;pci_write16(device,4,(pci_read(device,4)&0xffff)|5);
        outb(entropy_port+18,0);outb(entropy_port+18,1);outb(entropy_port+18,3);io_write32(entropy_port+4,0);
        outw(entropy_port+14,0);entropy_size=io_read16(entropy_port+12);
        if(!entropy_size||entropy_size>256){outb(entropy_port+18,128);return;}
        memset((void *)RNG_RING,0,16384);*(u16 *)(RNG_RING+16*entropy_size)=1;
        io_write32(entropy_port+8,RNG_RING/4096);outb(entropy_port+18,7);entropy_ready=1;
        serial("RNG: VirtIO hardware entropy queue ready\r\n");return;
    }
}
static i64 entropy_fill(void *output,u64 count){
    if(!entropy_ready)return -19;if(!count)return 0;u64 completed=0;
    while(completed<count){u32 n=count-completed>256?256:(u32)(count-completed);
        VirtioDescriptor *d=(void *)RNG_RING;d[0]=(VirtioDescriptor){RNG_DATA,n,2,0};
        volatile u16 *avail=(void *)(RNG_RING+16*entropy_size),*used=(void *)ring_used(RNG_RING,entropy_size);
        avail[2+entropy_avail%entropy_size]=0;__atomic_thread_fence(__ATOMIC_RELEASE);avail[1]=++entropy_avail;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);outw(entropy_port+16,0);
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
int aurora_net_transmit(const void *packet,unsigned length){
    if(!net_ready||!entropy_ready||length>1518||length<14)return 0;net_reclaim_tx();if(!net_ready)return 0;
    u32 id;for(id=0;id<net_tx_size&&net_tx_busy[id];id++){}if(id==net_tx_size)return 0;
    u8 *buffer=(void *)(NET_TX_DATA+id*NET_BUFFER);memset(buffer,0,10);memcpy(buffer+10,packet,length);
    VirtioDescriptor *d=(void *)NET_TX_RING;d[id]=(VirtioDescriptor){(u64)buffer,length+10,0,0};net_tx_busy[id]=1;
    volatile u16 *avail=(void *)(NET_TX_RING+16*net_tx_size);avail[2+net_tx_avail%net_tx_size]=id;
    __atomic_thread_fence(__ATOMIC_RELEASE);avail[1]=++net_tx_avail;__atomic_thread_fence(__ATOMIC_SEQ_CST);outw(net_port+16,1);return 1;
}
static void net_poll(void){
    if(!net_ready)return;net_reclaim_tx();if(!net_ready)return;
    volatile u16 *used=(void *)ring_used(NET_RX_RING,net_rx_size),*avail=(void *)(NET_RX_RING+16*net_rx_size);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if((u16)(used[1]-net_rx_used)>net_rx_size){net_bad_descriptors++;net_ready=0;return;}
    unsigned budget=64;int notify=0;
    while(net_rx_used!=used[1]&&budget--){volatile u32 *entry=(void *)((u64)used+4+8*(net_rx_used%net_rx_size));
        u32 id=entry[0],length=entry[1];if(id>=net_rx_size){net_bad_descriptors++;net_ready=0;return;}
        u8 *buffer=(void *)(NET_RX_DATA+id*NET_BUFFER);
        if(length>=24&&length<=1528&&!buffer[0]&&!buffer[1])network_input(buffer+10,length-10);else net_bad_descriptors++;
        net_rx_used++;avail[2+net_rx_avail%net_rx_size]=id;net_rx_avail++;notify=1;
    }
    if(notify){__atomic_thread_fence(__ATOMIC_RELEASE);avail[1]=net_rx_avail;__atomic_thread_fence(__ATOMIC_SEQ_CST);outw(net_port+16,0);}
    network_tick();if(!net_announced&&network_configured()){net_announced=1;serial("NET: DHCP IPv4 address, gateway and TCP/UDP ready\r\n");}
}
static void net_interrupt(u32 irq){
    if(!net_ready||(net_irq_mode?irq!=49:irq!=net_irq_line))return;
    if(!net_irq_mode&&!(inb(net_port+19)&3))return;net_interrupts++;
}
static void virtio_net_init(void){
    if(!entropy_ready)return;
    for(u32 bus=0;bus<256;bus++)for(u32 slot=0;slot<32;slot++){
        u32 device=(bus<<16)|(slot<<11);if(pci_read(device,0)!=0x10001af4)continue;
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
