/* A completion may precede delivery of its interrupt. Inspect the used ring
 * before declaring a timeout; resetting a completed device loses the volume. */
volatile u64 virtio_late_completions;
static int virtio_batch_done(void){
    volatile u16 *used=(volatile u16 *)((VIRTIO_RING+16*virtio_queue_size+6+2*virtio_queue_size+4095)&~4095ULL);
    return (u16)(used[1]-virtio_used)>=virtio_expected;
}
static void virtio_wake(void){tasks[virtio_waiter].state=RUNNABLE;virtio_waiter=-1;}
static void virtio_interrupt(u32 irq){
    if(!virtio_ready||(virtio_message_mode?irq!=48:irq!=virtio_irq_line))return;
    if(!virtio_message_mode){u8 status=inb(virtio_port+19);if(!(status&1))return;}virtio_interrupts++;
    if(virtio_waiter>=0&&virtio_batch_done())virtio_wake();
}
static void virtio_timeout(void){
    if(virtio_waiter>=0&&timer_ticks>=virtio_deadline){
        if(virtio_batch_done()){virtio_late_completions++;virtio_wake();return;}
        outb(virtio_port+18,0);virtio_ready=0;virtio_timeouts++;
        storage_ipc_cancel(&virtio_storage_broker,virtio_storage_sequence);
        serial("VIRTIO: incomplete block request timed out; volume disabled\r\n");
        virtio_wake();
    }
}
