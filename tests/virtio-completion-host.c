typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
__declspec(align(4096)) static u8 ring[8192];
#define VIRTIO_RING ((u64)ring)
#define RUNNABLE 0
static struct {int state;} tasks[2];
static int virtio_ready,virtio_message_mode,virtio_waiter;
static u32 virtio_irq_line,virtio_queue_size=8,virtio_port=0x100;
static u16 virtio_used,virtio_expected;
static u64 timer_ticks,virtio_deadline,virtio_interrupts,virtio_timeouts;
static u8 interrupt_status;
static int resets;
static u8 inb(u32 port){(void)port;return interrupt_status;}
static void outb(u32 port,u8 value){if(port==virtio_port+18&&!value)resets++;}
static void serial(const char *s){(void)s;}
#include "../src/virtio_completion.h"
static void fixture(u16 completed){
    virtio_ready=1;virtio_message_mode=2;virtio_waiter=1;tasks[1].state=5;
    virtio_used=65535;virtio_expected=2;timer_ticks=99;virtio_deadline=100;
    virtio_interrupts=virtio_timeouts=virtio_late_completions=0;resets=0;
    ((u16 *)(ring+4096))[1]=completed;
}
__declspec(dllexport) int completion_test(void){
    fixture(1);virtio_timeout();if(tasks[1].state!=5||resets)return 1;
    timer_ticks=100;virtio_timeout();
    if(!virtio_ready||resets||virtio_timeouts||virtio_late_completions!=1||virtio_waiter!=-1||tasks[1].state!=RUNNABLE)return 2;
    fixture(0);timer_ticks=100;virtio_timeout();
    if(virtio_ready||resets!=1||virtio_timeouts!=1||virtio_late_completions||virtio_waiter!=-1)return 3;
    fixture(1);virtio_interrupt(49);if(virtio_waiter!=1)return 4;
    virtio_interrupt(48);if(virtio_waiter!=-1||virtio_interrupts!=1||resets)return 5;
    fixture(0);virtio_interrupt(48);if(virtio_waiter!=1)return 6;
    fixture(1);virtio_message_mode=0;virtio_irq_line=11;interrupt_status=0;
    virtio_interrupt(11);if(virtio_waiter!=1||virtio_interrupts)return 7;
    interrupt_status=1;virtio_interrupt(11);if(virtio_waiter!=-1||virtio_interrupts!=1)return 8;
    return 0;
}
