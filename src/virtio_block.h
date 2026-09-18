/* Transitional VirtIO: bounded DMA request, IRQ completion and blocked caller.
 * Filesystem serialization owns the queue; early boot uses bounded polling. */
static u16 virtio_port,virtio_queue_size,virtio_avail,virtio_used;
static u64 virtio_sectors;
static u32 virtio_features;
static int virtio_ready,virtio_present;
static int virtio_message_mode;
static u32 virtio_irq_line;
static int virtio_waiter=-1;
static u64 virtio_deadline;
volatile u64 virtio_interrupts,virtio_suspensions,virtio_timeouts;
static u32 io_read32(u16 port){u32 value;__asm__ volatile("inl %1,%0":"=a"(value):"Nd"(port));return value;}
static u16 io_read16(u16 port){u16 value;__asm__ volatile("inw %1,%0":"=a"(value):"Nd"(port));return value;}
static void io_write32(u16 port,u32 value){__asm__ volatile("outl %0,%1"::"a"(value),"Nd"(port));}
static u32 pci_read(u32 address,u32 reg){io_write32(0xcf8,0x80000000U|address|reg);return io_read32(0xcfc);}
#include "pci_irq.h"
typedef struct {u64 address;u32 length;u16 flags,next;} VirtioDescriptor;
#define VIRTIO_RING 0x0d010000ULL
#define VIRTIO_REQUEST 0x0d020000ULL
#define VIRTIO_BOUNCE 0x0d000000ULL
static void virtio_block_init(void){
    for(u32 bus=0;bus<256;bus++)for(u32 slot=0;slot<32;slot++){
        u32 address=(bus<<16)|(slot<<11);if(pci_read(address,0)!=0x10011af4)continue;
        u32 bar=pci_read(address,0x10);if(!(bar&1)||bar>65535)continue;
        virtio_present=1;virtio_port=bar&~3U;io_write32(0xcf8,0x80000000U|address|4);io_write32(0xcfc,(pci_read(address,4)&0xffff)|5);
        outb(virtio_port+18,0);outb(virtio_port+18,1);outb(virtio_port+18,3);
        virtio_features=io_read32(virtio_port)&(1U<<9);io_write32(virtio_port+4,virtio_features);
        outw(virtio_port+14,0);virtio_queue_size=io_read16(virtio_port+12);
        if(virtio_queue_size<3||virtio_queue_size>256){outb(virtio_port+18,128);return;}
        memset((void *)VIRTIO_RING,0,16384);virtio_irq_line=pci_read(address,0x3c)&255;
        io_write32(virtio_port+8,VIRTIO_RING/4096);
        virtio_message_mode=pci_message_irq(address,48);
        if(virtio_message_mode==2){outw(virtio_port+20,0xffff);outw(virtio_port+22,0);
            if(io_read16(virtio_port+22)==0xffff){pci_write16(address,pci_msix_cap+2,(pci_read(address,pci_msix_cap)>>16)&~0x8000);pci_write16(address,4,(pci_read(address,4)&0xffff)&~0x400);virtio_message_mode=0;}}
        u32 config=virtio_message_mode==2?24:20;
        virtio_sectors=io_read32(virtio_port+config)|((u64)io_read32(virtio_port+config+4)<<32);
        outb(virtio_port+18,7);virtio_ready=1;
        serial("VIRTIO: PCI block queue ready sectors=");hex(virtio_sectors);serial(virtio_message_mode==2?" MSI-X\r\n":virtio_message_mode?" MSI\r\n":" INTx\r\n");return;
    }
}
static void virtio_interrupt(u32 irq){
    if(!virtio_ready||(virtio_message_mode?irq!=48:irq!=virtio_irq_line))return;
    if(!virtio_message_mode){u8 status=inb(virtio_port+19);if(!(status&1))return;}virtio_interrupts++;
    if(virtio_waiter>=0){tasks[virtio_waiter].state=RUNNABLE;virtio_waiter=-1;}
}
static void virtio_timeout(void){
    if(virtio_waiter>=0&&timer_ticks>=virtio_deadline){
        outb(virtio_port+18,0);virtio_ready=0;virtio_timeouts++;
        tasks[virtio_waiter].state=RUNNABLE;virtio_waiter=-1;
    }
}
static int virtio_transfer(u64 sector,void *data,u32 count,int operation){
    if(!virtio_ready||count>128||sector>virtio_sectors||count>virtio_sectors-sector)return 0;
    if(operation==4&&!(virtio_features&(1U<<9)))return 0;
    if(operation==1)memcpy((void *)VIRTIO_BOUNCE,data,(u64)count*512);
    *(u32 *)VIRTIO_REQUEST=operation;*(u32 *)(VIRTIO_REQUEST+4)=0;*(u64 *)(VIRTIO_REQUEST+8)=sector;
    *(volatile u8 *)(VIRTIO_REQUEST+16)=255;
    VirtioDescriptor *desc=(VirtioDescriptor *)VIRTIO_RING;
    desc[0]=(VirtioDescriptor){VIRTIO_REQUEST,16,1,operation==4?2:1};
    desc[1]=(VirtioDescriptor){VIRTIO_BOUNCE,count*512,operation==0?3:1,2};
    desc[2]=(VirtioDescriptor){VIRTIO_REQUEST+16,1,2,0};
    volatile u16 *avail=(volatile u16 *)(VIRTIO_RING+16*virtio_queue_size);
    u64 used_address=(VIRTIO_RING+16*virtio_queue_size+6+2*virtio_queue_size+4095)&~4095ULL;
    volatile u16 *used=(volatile u16 *)used_address;
    avail[2+virtio_avail%virtio_queue_size]=0;__asm__ volatile("mfence":::"memory");avail[1]=++virtio_avail;
    __asm__ volatile("mfence":::"memory");outw(virtio_port+16,0);
    if(kernel_started&&(virtio_message_mode||(virtio_irq_line>0&&virtio_irq_line<16))){
        while(used[1]==virtio_used&&virtio_ready){virtio_waiter=current_task;virtio_deadline=timer_ticks+200;
            tasks[current_task].state=WAIT_IO;virtio_suspensions++;kernel_suspend();}
        if(!virtio_ready)return 0;
    }else{
        u32 spins=100000000;while(used[1]==virtio_used&&--spins)__asm__ volatile("pause":::"memory");
        if(!spins){outb(virtio_port+18,0);virtio_ready=0;return 0;}
    }
    __asm__ volatile("mfence":::"memory");virtio_used=used[1];(void)inb(virtio_port+19);
    if(*(volatile u8 *)(VIRTIO_REQUEST+16))return 0;
    if(operation==0)memcpy(data,(void *)VIRTIO_BOUNCE,(u64)count*512);
    return 1;
}
