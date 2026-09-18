/* Aurora microkernel: address spaces, traps, scheduling, IPC and capabilities.
 * Desktop/input/display run in ring 3; storage remains a kernel compatibility path. */
#include "abi.h"
#include "images.h"
#include "network_api.h"
#include "radeon_service.h"
static int radeon_present;
static u16 radeon_device;
static u32 radeon_bar0, radeon_irq, radeon_generation;
static u64 radeon_mmio, radeon_vram;
static int radeon_flr, radeon_irq_cap;
#define NX (1ULL<<63)
#define PRESENT 1ULL
#define WRITE 2ULL
#define USER 4ULL
#define HUGE 128ULL
#ifdef AURORA_SELF_TEST
#define APP_FIRST 10
#else
#define APP_FIRST 3
#endif
#define TASK_COUNT TASK_LIMIT
#define QUEUE_SIZE 32
/* Large per-task tables live in reserved supervisor RAM below 128 MiB rather
 * than in the kernel image's bounded .bss. They are zeroed at startup. Kernel
 * code runs under the kernel root (CR3 0x200000), where this region is an
 * identity huge-page mapping; task roots need not map it because the trap
 * return path copies the outgoing frame onto the task's kernel stack before
 * switching CR3. Kernel stacks, in contrast, are used by the trap entry under
 * the task's own CR3, so every root maps them. */
#define KERNEL_STATE 0x04000000ULL
#define KERNEL_STATE_SIZE 0x80000ULL
/* Kernel stacks for slots 16-31 share one 4 KiB-granular table with a guard.
 * Their virtual window lies outside every user range (native user addresses
 * end at 0x203fffff); the backing RAM sits below 128 MiB so the smallest
 * supported machine has it. */
#define EXTENDED_STACKS 0x3f000000ULL
#define EXTENDED_STACKS_PHYSICAL 0x04100000ULL
#define EXTENDED_STACK_TABLE 0x209000ULL
enum { RUNNABLE, WAITING, DEAD, WAIT_VFORK, STOPPED, WAIT_IO, WAIT_FS, WAIT_EVENT };
/* Exactly the stack layout in traps.asm, including the hardware IRET frame. */
typedef struct {
    u64 r15,r14,r13,r12,r11,r10,r9,r8,rsi,rdi,rbp,rdx,rcx,rbx,rax;
    u64 vector,error,rip,cs,flags,rsp,ss;
} Frame;
_Static_assert(sizeof(Frame)==176,"traps.asm pushes a 176-byte frame");
typedef struct {
    Frame frame;
    Message queue[QUEUE_SIZE];
    u64 receive_address;
    u32 head,count,state;
} Task;
typedef struct __attribute__((packed)) {
    u32 reserved; u64 rsp[3],reserved1,ist[7],reserved2;
    u16 reserved3,iomap;
} Tss;
typedef struct __attribute__((packed)) { u16 limit; u64 base; } TablePointer;
typedef struct __attribute__((packed)) {
    u16 low,selector; u8 ist,attributes; u16 middle; u32 high,reserved;
} Gate;
#define tasks ((Task *)KERNEL_STATE)
#define task_fp ((u8 (*)[512])(KERNEL_STATE+0x10000))
_Static_assert(sizeof(Task)*TASK_COUNT<=0x10000,"task table exceeds its reservation");
static i64 exit_codes[TASK_COUNT];
static int native_active[TASK_COUNT];
static u64 task_fsbase[TASK_COUNT],task_gsbase[TASK_COUNT];
static u8 initial_fp[512] __attribute__((aligned(16)));
static u64 kernel_stack(u32 id){return id<LEGACY_TASKS?0x100000ULL+(id+1)*0x10000ULL:EXTENDED_STACKS+(id-LEGACY_TASKS+1)*0x10000ULL;}
static void *native_buffer(u32 id,u64 address,u64 size,int write);
static int native_signal_deliver(u32 id);
static void native_finish(u32 id,i64 code);
static void native_timers(void);
static void native_wake_waiters(void);
static void net_poll(void);
static Gate idt[256];

/* Named counters are also consumed by the QMP integration tests. */
volatile u64 timer_ticks,ipc_messages,context_switches;
volatile u64 task_runs[TASK_COUNT],task_faults[TASK_COUNT],task_cr3[TASK_COUNT];
volatile u64 task_preemptions[TASK_COUNT],task_fault_addresses[TASK_COUNT];
u64 task_kernel_sp[TASK_COUNT];
#include "cpu.h"
static int kernel_started,filesystem_owner=-1;
extern void kernel_suspend(void);
extern void *isr_table[];
extern void install_gdt(TablePointer *);
extern void enter_user(Frame *) __attribute__((noreturn));
extern void syscall_entry(void);
static inline void outb(u16 p,u8 v) { __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline u8 inb(u16 p) { u8 v;__asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p));return v; }
static inline void outw(u16 p,u16 v) { __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
void *memset(void *p,int v,u64 n) { u8 *s=p;while(n--)*s++=(u8)v;return p; }
void *memcpy(void *d,const void *s,u64 n) { u8 *a=d;const u8 *b=s;while(n--)*a++=*b++;return d; }
static void serial(const char *s) { while(*s){while(!(inb(0x3fd)&32)){}outb(0x3f8,*s++);} }
static void hex(u64 n) { const char *digits="0123456789abcdef";for(int i=60;i>=0;i-=4){char s[2]={digits[(n>>i)&15],0};serial(s);} }
static void panic(const char *s) { serial("KERNEL PANIC: ");serial(s);for(;;)__asm__ volatile("cli; hlt"); }
static void wrmsr(u32 msr,u64 value){__asm__ volatile("wrmsr"::"c"(msr),"a"((u32)value),"d"((u32)(value>>32)));}
void save_context(void){u32 lo,hi;__asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(0xc0000102));task_gsbase[current_task]=lo|((u64)hi<<32);__asm__ volatile("fxsave64 %0":"=m"(task_fp[current_task]));}
/* Prepare the return to the current task. The task frame lives in
 * KERNEL_STATE, which only the kernel root maps, so it is copied to the top of
 * the task's kernel stack (mapped in every root, and exactly where the trap
 * entry pushed it). The task's CR3 travels in the copy's error-code slot,
 * which the return path discards. The caller may still be running on this
 * very stack below the frame, so nothing outside the frame is written. The
 * assembly return path releases the CPU, loads that CR3 and pops the copy. */
u64 restore_context(void){
    kernel_stack_top=kernel_stack(current_task);cpu_local()->tss.rsp[0]=kernel_stack_top;
    wrmsr(0xc0000100,task_fsbase[current_task]);wrmsr(0xc0000102,task_gsbase[current_task]);
    __asm__ volatile("fxrstor64 %0"::"m"(task_fp[current_task]));
    Frame *copy=(Frame *)(kernel_stack_top-sizeof(Frame));memcpy(copy,&tasks[current_task].frame,sizeof(Frame));copy->error=task_cr3[current_task];
    return (u64)copy;
}
static u64 physical(u32 id) { return 0x2000000ULL+(u64)id*USER_SIZE; }
static u64 *user_table(u32 id) { return (u64 *)(task_cr3[id]+0x6000); }
static void tables_init(u32 index) {
    Cpu *cpu=&cpus[index];cpu->index=index;cpu->task=TASK_COUNT;cpu->stack=cpu->idle_stack=index?0x00300000ULL+(index+1)*0x10000ULL:0x90000;
    Tss *tp=&cpu->tss;u64 *gdt=cpu->gdt;
#define tss (*tp)
    gdt[1]=0x00af9a000000ffffULL;gdt[2]=0x00cf92000000ffffULL;
    gdt[3]=0x00cff2000000ffffULL;gdt[4]=0x00affa000000ffffULL;
    tss.rsp[0]=0x90000;
    tss.iomap=sizeof(tss); /* Beyond the limit: ring 3 cannot execute port I/O. */
    u64 base=(u64)&tss,limit=sizeof(tss)-1;
    gdt[5]=(limit&0xffff)|((base&0xffffff)<<16)|(0x89ULL<<40)|((base&0xff000000)<<32);
    gdt[6]=base>>32;
    TablePointer gp={sizeof(cpu->gdt)-1,(u64)gdt};install_gdt(&gp);
    wrmsr(0xc0000101,(u64)cpu);wrmsr(0xc0000102,0);
    if(!index)for(int i=0;i<=64;i++) {
        int vector=i==64?128:i;u64 addr=(u64)isr_table[i];
        idt[vector]=(Gate){addr&0xffff,8,0,i==64?0xee:0x8e,(addr>>16)&0xffff,addr>>32,0};
    }
    TablePointer ip={sizeof(idt)-1,(u64)idt};__asm__ volatile("lidt %0"::"m"(ip));
    u32 lo,hi;__asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(0xc0000080));
    lo|=(1<<11)|1;__asm__ volatile("wrmsr"::"a"(lo),"d"(hi),"c"(0xc0000080));
    wrmsr(0xc0000081,((u64)8<<32)|((u64)0x13<<48));
    wrmsr(0xc0000082,(u64)syscall_entry);wrmsr(0xc0000084,0x700);
    u64 cr4;__asm__ volatile("mov %%cr4,%0":"=r"(cr4));cr4|=(1<<9)|(1<<10);__asm__ volatile("mov %0,%%cr4"::"r"(cr4));
    u64 cr0;__asm__ volatile("mov %%cr0,%0":"=r"(cr0));cr0=(cr0|1<<16|2)&~12ULL;
    __asm__ volatile("mov %0,%%cr0"::"r"(cr0):"memory");
    if(!index)__asm__ volatile("fninit; fxsave64 %0":"=m"(initial_fp));
#undef tss
}
static void create_task(u32 id,const u8 *image,u64 size,u64 text_end,u64 ro_end,u64 framebuffer) {
    memset(&tasks[id],0,sizeof(Task));
    native_active[id]=0;task_affinity[id]=1;task_fsbase[id]=task_gsbase[id]=0;memcpy(task_fp[id],initial_fp,512);
    u64 root=0x200000+(u64)id*0x10000;task_cr3[id]=root;
    memset((void *)root,0,0x10000);
    u64 *pml4=(u64 *)root,*pdpt=(u64 *)(root+0x1000),*pd=(u64 *)(root+0x2000);
    pml4[0]=(root+0x1000)|7;
    for(int i=0;i<4;i++)pdpt[i]=(root+0x2000+i*0x1000)|7;
    /* Supervisor identity mappings allow kernel copies into task RAM.
       Only explicit grants below get the U/S bit at the leaf level. */
    for(int i=0;i<2048;i++)pd[i]=(u64)i*0x200000|PRESENT|WRITE|HUGE;
    if(id==0){u64 *low=(u64 *)0x207000;for(int i=0;i<512;i++)low[i]=(u64)i*4096|3;
        for(int i=0;i<LEGACY_TASKS;i++)low[256+i*16]=0;
        u64 *idle=(u64 *)0x208000;for(int i=0;i<512;i++)idle[i]=(0x200000ULL+(u64)i*4096)|3;
        for(int i=0;i<CPU_MAX;i++)idle[256+i*16]=0;
        u64 *extended=(u64 *)EXTENDED_STACK_TABLE;for(int i=0;i<512;i++)extended[i]=(EXTENDED_STACKS_PHYSICAL+(u64)i*4096)|3;
        for(int i=0;i<TASK_COUNT-LEGACY_TASKS;i++)extended[i*16]=0;}
    pd[1]=0x208003;pd[0]=0x207003; /* Shared supervisor mappings with a guard per kernel stack. */
    pd[EXTENDED_STACKS/0x200000]=EXTENDED_STACK_TABLE|3;
    pd[USER_BASE/0x200000]=(root+0x6000)|7;
    u64 *pt=user_table(id);
    for(int i=0;i<512;i++) {
        u64 va=USER_BASE+(u64)i*4096,flags=PRESENT|USER|WRITE|NX;
        if(va<text_end)flags=PRESENT|USER;
        else if(va<ro_end)flags=PRESENT|USER|NX;
        if(va>=BOOT_ADDRESS&&va<BOOT_ADDRESS+8192)flags=PRESENT|USER|NX;
        if(va==0x5ef000)flags=0; /* Guard below the 64 KiB user stack. */
        pt[i]=(physical(id)+(u64)i*4096)|flags;
    }
    if(id==DESKTOP||id==DISPLAY)
        for(int i=8;i<10;i++)pd[i]=(u64)i*0x200000|PRESENT|USER|HUGE|NX|(id==DESKTOP?WRITE:0);
    if(id==DISPLAY&&radeon_present)
        pd[RADEON_RING_ADDRESS/0x200000]=(RADEON_RING_ADDRESS/0x200000)*0x200000ULL|PRESENT|USER|WRITE|HUGE|NX;
    if(id==DISPLAY) {
        u64 first=framebuffer/0x200000,last=(framebuffer+SURFACE_BYTES-1)/0x200000;
        for(u64 i=first;i<=last;i++)pd[i]=i*0x200000|PRESENT|WRITE|USER|HUGE|NX;
    }
    memset((void *)physical(id),0,USER_SIZE);memcpy((void *)physical(id),image,size);
    BootInfo *boot=(BootInfo *)(physical(id)+BOOT_ADDRESS-USER_BASE);
    boot->id=id;
    if(id==DISPLAY){boot->framebuffer=framebuffer;boot->pitch=*(volatile u16 *)0x910/4;
        boot->display_features=radeon_present?1:0;boot->radeon_mmio=radeon_mmio;boot->radeon_vram=radeon_vram;
        boot->radeon_irq=radeon_irq_cap?radeon_irq:0;boot->gpu_ring=radeon_present?RADEON_RING_ADDRESS:0;
        boot->radeon_device=radeon_device;boot->radeon_generation=radeon_generation;boot->gpu_ring_entries=RADEON_RING_ENTRIES;}
    if(id==DESKTOP)memcpy(boot->font,(const void *)0x70000,4096);
    tasks[id].frame=(Frame){.rip=USER_BASE,.cs=0x23,.flags=0x202,.rsp=0x600000,.ss=0x1b};
    serial("TASK ring3 id=");hex(id);serial(" cr3=");hex(root);serial("\r\n");
}
/* Check every page; reject overflow before addition. Buffers must be private. */
static void *user_buffer(u32 id,u64 address,u64 size,int write) {
    if(native_active[id])return native_buffer(id,address,size,write);
    if(!size||address<USER_BASE||address>=USER_BASE+USER_SIZE||size>USER_BASE+USER_SIZE-address)return 0;
    for(u64 p=(address-USER_BASE)/4096;p<=(address+size-1-USER_BASE)/4096;p++) {
        u64 flags=user_table(id)[p];
        if((flags&5)!=5||(write&&!(flags&WRITE)))return 0;
    }
    return (void *)(physical(id)+address-USER_BASE);
}
static int application_slot_available(u32 id);
#include "storage.h"
static int endpoint_allowed(u32 from,u64 to) {
    return (from==DESKTOP&&(to==INPUT||to==DISPLAY)) || ((from==INPUT||from==DISPLAY||from>=APP_FIRST)&&to==DESKTOP);
}
static i64 ipc_send(u64 to,u64 address) {
    if(!endpoint_allowed(current_task,to))return ERR_CAP;
    Message *source=user_buffer(current_task,address,sizeof(Message),0);
    if(!source)return ERR_POINTER;
    Task *target=&tasks[to];if(target->state==DEAD)return ERR_DEAD;
    Message m=*source;m.sender=current_task;
    if(target->state==WAITING) {
        void *destination=user_buffer(to,target->receive_address,sizeof(m),1);
        if(!destination)panic("Invalid saved receive buffer");
        memcpy(destination,&m,sizeof(m));target->frame.rax=0;target->state=RUNNABLE;
    } else {
        if(target->count==QUEUE_SIZE)return ERR_FULL;
        target->queue[(target->head+target->count)%QUEUE_SIZE]=m;target->count++;
    }
    ipc_messages++;return 0;
}
static i64 ipc_receive(u64 address,int block) {
    Message *destination=user_buffer(current_task,address,sizeof(Message),1);
    if(!destination)return ERR_POINTER;
    Task *t=&tasks[current_task];
    if(t->count){*destination=t->queue[t->head];t->head=(t->head+1)%QUEUE_SIZE;t->count--;return 0;}
    if(!block)return ERR_EMPTY;
    t->state=WAITING;t->receive_address=address;return 0;
}
static int port_allowed(u64 port,u64 width,int read) {
    if(current_task!=INPUT)return 0;
    if(width==1&&(port==0x60||port==0x64||port==0x70||port==0x71))return 1;
    return !read&&width==2&&port==0x604;
}
Frame *schedule(void) {
    compatibility_enter();
    net_poll();
    native_timers();native_wake_waiters();
    u32 old=current_task;u32 cpu=cpu_local()->index;
    if(old<TASK_COUNT)task_cpu[old]=-1;
    for(u32 offset=1;offset<=TASK_COUNT;offset++) {
        u32 next=(old+offset)%TASK_COUNT;spin_lock(&ipc_locks[next]);
        if(tasks[next].state==RUNNABLE&&task_cpu[next]<0&&(task_affinity[next]&(1U<<cpu))&&(!cpu||native_active[next])) {
            current_task=next;task_cpu[next]=cpu;task_runs[next]++;if(next!=old)context_switches++;
            spin_unlock(&ipc_locks[next]);return &tasks[next].frame;
        }
        spin_unlock(&ipc_locks[next]);
    }
    current_task=TASK_COUNT;return 0;
}
static void filesystem_enter(void){
    while(filesystem_owner>=0&&filesystem_owner!=(int)current_task){tasks[current_task].state=WAIT_FS;kernel_suspend();}
    filesystem_owner=current_task;
}
static void filesystem_leave(void){
    filesystem_owner=-1;for(int i=0;i<TASK_COUNT;i++)if(tasks[i].state==WAIT_FS)tasks[i].state=RUNNABLE;
}
#include "native.h"
#include "radeon.h"
Frame *final_context(Frame *frame){
    /* A syscall may select its own task on an otherwise idle AP. Deliver
       pending signals after releasing the filesystem mutex, before IRET. */
    while(frame&&native_active[current_task]&&!task_kernel_sp[current_task]&&filesystem_owner<0){
        NativeSignals *s=native_signals(current_task);NativeProcess *p=&native_process[current_task];
        if(tasks[current_task].state==RUNNABLE&&!p->restore_mask_valid&&!(s->pending&~p->sigmask))break;
        compatibility_enter();if(filesystem_owner>=0)break;native_signal_deliver(current_task);
        if(tasks[current_task].state==RUNNABLE)break;
        frame=schedule();
    }
    return frame;
}
Frame *trap_dispatch(Frame *frame);
#include "smp.h"
Frame *trap_dispatch(Frame *frame) {
    if((frame->cs&3)!=3){serial("vector=");hex(frame->vector);serial(" rip=");hex(frame->rip);panic(" supervisor exception\r\n");}
    Task *t=&tasks[current_task];t->frame=*frame;
    /* Read-only per-task queries do not take the native domain lock. Keep the
       read-side marker set until IRET so teardown waits for this CPU. */
    if(native_active[current_task]&&frame->vector==128){
        u64 n=frame->rax;
        if(n==39||n==186||n==102||n==104||n==107||n==108){
            t->frame.rax=n==39?native_process[current_task].tgid:n==186?current_task+100:1000;
            cpu_fast_calls[cpu_local()->index]++;return &t->frame;
        }
    }
    __atomic_store_n(&cpu_local()->in_user,0,__ATOMIC_RELEASE);
    int service_fast=!native_active[current_task]&&frame->vector==128&&frame->rax<=SYS_TICKS;
    if(service_fast&&__atomic_load_n(&compatibility_lock,__ATOMIC_ACQUIRE))cpu_parallel_service_calls++;
    if(!service_fast)compatibility_enter();
    if(t->state==DEAD||t->state==STOPPED)return schedule();
    if(frame->vector==62){cpu_rendezvous[cpu_local()->index]++;if(local_apic)local_apic[0xb0/4]=0;return schedule();}
    if(frame->vector==32) { timer_ticks++;for(u32 i=1;i<cpu_count;i++)if(cpus[i].online)cpu_ipi(cpus[i].apic_id,62);virtio_timeout();ata_timeout();task_preemptions[current_task]++;outb(0x20,0x20);return schedule(); }
    if(frame->vector>=33&&frame->vector<48){virtio_interrupt(frame->vector-32);net_interrupt(frame->vector-32);ata_interrupt(frame->vector-32);if(frame->vector>=40)outb(0xa0,0x20);outb(0x20,0x20);return schedule();}
    if(frame->vector>=48&&frame->vector<64){if(frame->vector!=63){virtio_interrupt(frame->vector);net_interrupt(frame->vector);if(local_apic)local_apic[0xb0/4]=0;}return schedule();}
    if(frame->vector!=128) {
        u64 address=0;if(frame->vector==14)__asm__ volatile("mov %%cr2,%0":"=r"(address));
        if(frame->vector==14&&(native_write_fault(current_task,address,frame->error)||native_lazy_fault(current_task,address,frame->error)))return &t->frame;
        if(native_fault_signal(current_task,frame->vector,address))return &t->frame; /* final_context delivers the handler */
        task_faults[current_task]=frame->vector+1;task_fault_addresses[current_task]=address;t->state=DEAD;exit_codes[current_task]=-128-(i64)frame->vector;
        if(native_active[current_task])native_mark_group(current_task,exit_codes[current_task]);
        serial("FAULT isolated task=");hex(current_task);serial(" vector=");hex(frame->vector);
        serial(" address=");hex(address);serial(" error=");hex(frame->error);serial(" rip=");hex(frame->rip);serial("\r\n");
        return schedule();
    }
    if(!service_fast)native_reap_pending();
    if(native_active[current_task]){
        /* Wait queues, clocks, affinity and thread bookkeeping do not acquire
           the filesystem mutex. Mapping/lifetime and descriptor changes still
           do, since a sleeping driver may retain an alias into their buffers. */
        u64 n=frame->rax;int needs_fs=!(n==7||n==23||n==24||n==35||n==96||n==115||n==131||n==202||n==203||n==204||n==218||n==228||n==229||n==230||n==270||n==271||n==273||n==274||n==309);
        if((n>=41&&n<=55)||n==318||n==284||n==290)needs_fs=0;
        if(n==34||n==130||n==127||n==128||n==129||n==297||n==36||n==37||n==38||n==100||(n>=140&&n<=148)||n==157||n==160||n==324||n==13||n==14)needs_fs=0;
        if((n==0||n==1||n==3||n==5||n==16||n==19||n==20||n==72)&&frame->rdi<NATIVE_FDS&&native_process[current_task].fd[frame->rdi].kind>=6)needs_fs=0;
        if(needs_fs)filesystem_enter();Frame *next=native_dispatch(frame);if(needs_fs)filesystem_leave();return next;
    }
    /* Legacy AuroraFS calls also sleep in interrupt-driven ATA now, so they
       share the filesystem mutex and its kernel continuation. */
    int fs_call=frame->rax==SYS_NATIVE_SPAWN||frame->rax==SYS_NATIVE_READ||frame->rax==SYS_NATIVE_WRITE||frame->rax==SYS_NATIVE_LIST||frame->rax==SYS_SYNC
        ||frame->rax==SYS_FILE_READ||frame->rax==SYS_FILE_WRITE||frame->rax==SYS_SPAWN;
    if(fs_call)filesystem_enter();
    i64 result=0;int reschedule=0;
    switch(frame->rax) {
    case SYS_YIELD: reschedule=1;break;
    case SYS_SEND: if(frame->rdi>=TASK_COUNT){result=ERR_CAP;break;}spin_lock(&ipc_locks[frame->rdi]);result=ipc_send(frame->rdi,frame->rsi);spin_unlock(&ipc_locks[frame->rdi]);break;
    case SYS_RECV: spin_lock(&ipc_locks[current_task]);result=ipc_receive(frame->rdi,1);reschedule=t->state==WAITING;spin_unlock(&ipc_locks[current_task]);break;
    case SYS_POLL: spin_lock(&ipc_locks[current_task]);result=ipc_receive(frame->rdi,0);spin_unlock(&ipc_locks[current_task]);break;
    case SYS_IN: result=port_allowed(frame->rdi,1,1)?inb(frame->rdi):ERR_CAP;break;
    case SYS_OUT:
        if(!port_allowed(frame->rdi,frame->rdx,0))result=ERR_CAP;
        else if(frame->rdx==1)outb(frame->rdi,frame->rsi);else outw(frame->rdi,frame->rsi);
        break;
    case SYS_LOG: {
        char *s=frame->rsi<=160?user_buffer(current_task,frame->rdi,frame->rsi,0):0;
        if(!s)result=ERR_POINTER;
        else for(u64 i=0;i<frame->rsi;i++){while(!(inb(0x3fd)&32)){}outb(0x3f8,s[i]);}
        break;
    }
    case SYS_TICKS:result=timer_ticks;break;
    case SYS_EXIT:exit_codes[current_task]=(i64)frame->rdi;t->state=DEAD;reschedule=1;break;
    case SYS_FILE_READ:case SYS_FILE_WRITE:result=file_transfer(frame->rdi,frame->rax==SYS_FILE_WRITE);break;
    case SYS_FILE_LIST: {
        FileEntry *entry=user_buffer(current_task,frame->rsi,sizeof(FileEntry),1);
        if(!entry)result=ERR_POINTER;
        else if(!fs_ready)result=ERR_IO;
        else if(frame->rdi>=FS_FILES)result=ERR_LIMIT;
        else {*entry=directory[frame->rdi];result=0;}
        break;
    }
    case SYS_SPAWN:result=current_task==DESKTOP?spawn_application(frame->rdi):ERR_CAP;break;
    case SYS_STATUS: {
        i64 *code=user_buffer(current_task,frame->rsi,sizeof(i64),1);
        if(current_task!=DESKTOP)result=ERR_CAP;
        else if(frame->rdi<APP_FIRST||frame->rdi>=TASK_COUNT)result=ERR_LIMIT;
        else if(!code)result=ERR_POINTER;
        else {*code=exit_codes[frame->rdi];result=tasks[frame->rdi].state==DEAD&&(!native_active[frame->rdi]||!native_group_refs[frame->rdi]);}
        break;
    }
    case SYS_NATIVE_SPAWN:result=current_task==DESKTOP?native_spawn(frame->rdi):ERR_CAP;break;
    case SYS_NATIVE_READ:case SYS_NATIVE_WRITE:result=current_task==DESKTOP?native_shell_file(frame->rdi,frame->rax==SYS_NATIVE_WRITE):ERR_CAP;break;
    case SYS_NATIVE_LIST:result=current_task==DESKTOP?native_shell_list(frame->rdi,frame->rsi):ERR_CAP;break;
    case SYS_NATIVE_INPUT:result=current_task==DESKTOP?native_terminal_input(frame->rdi,frame->rsi):ERR_CAP;break;
    case SYS_SYNC:result=current_task==DESKTOP?native_sync():ERR_CAP;break;
    default:result=ERR_SYSCALL;
    }
    if(fs_call)filesystem_leave();t->frame.rax=result;return reschedule?schedule():&t->frame;
}
static void timer_init(void) {
    /* Only PIT IRQ0 is unmasked; PS/2 is polled by the input service. */
    outb(0x20,0x11);outb(0xa0,0x11);outb(0x21,32);outb(0xa1,40);
    outb(0x21,4);outb(0xa1,2);outb(0x21,1);outb(0xa1,1);
    outb(0x21,0xfe);outb(0xa1,0xff);
    outb(0x43,0x36);outb(0x40,11932&255);outb(0x40,11932>>8);
    if(virtio_ready&&!virtio_message_mode&&virtio_irq_line>0&&virtio_irq_line<16){u16 mask=0xfffe;mask&=~(1U<<virtio_irq_line);if(virtio_irq_line>=8)mask&=~4U;outb(0x21,mask);outb(0xa1,mask>>8);}
    if(net_ready&&!net_irq_mode&&net_irq_line>0&&net_irq_line<16){u16 mask=inb(0x21)|((u16)inb(0xa1)<<8);mask&=~(1U<<net_irq_line);if(net_irq_line>=8)mask&=~4U;outb(0x21,mask);outb(0xa1,mask>>8);}
    /* Primary ATA completion (IRQ14) drives AuroraFS and the legacy development
       volume once tasks can sleep. The drive's nIEN bit stays clear. */
    {u16 mask=inb(0x21)|((u16)inb(0xa1)<<8);mask&=~(1U<<14);mask&=~4U;outb(0x21,mask);outb(0xa1,mask>>8);outb(0x3f6,0);(void)inb(0x1f7);ata_irq_mode=1;}
}
void kernel_main(void) {
    outb(0x3f9,0);outb(0x3fb,0x80);outb(0x3f8,1);outb(0x3f9,0);
    outb(0x3fb,3);outb(0x3fa,0xc7);outb(0x3fc,0x0b);
    serial("AURORA: microkernel 0.2 / 64-bit\r\n");memset((void *)KERNEL_STATE,0,KERNEL_STATE_SIZE);tables_init(0);
    for(int i=0;i<TASK_COUNT;i++){tasks[i].state=DEAD;task_cpu[i]=-1;task_affinity[i]=1;}
    native_clock_init();
    virtio_block_init();
    filesystem_init();
    native_fs_init();
    u64 fb=*(volatile u32 *)0x928;
    if(fb<0x80000000||fb+SURFACE_BYTES>0x100000000ULL||*(volatile u16 *)0x910!=4096)panic("Unsupported framebuffer layout");
    radeon_init();
    create_task(DESKTOP,desktop_image,desktop_image_size,DESKTOP_TEXT_END,DESKTOP_RO_END,fb);
    create_task(INPUT,input_image,input_image_size,INPUT_TEXT_END,INPUT_RO_END,fb);
    create_task(DISPLAY,display_image,display_image_size,DISPLAY_TEXT_END,DISPLAY_RO_END,fb);
    native_memory_init();
    if(native_ready){entropy_init();virtio_net_init();}
#ifdef AURORA_SELF_TEST
    for(int i=3;i<10;i++)create_task(i,probe_image,probe_image_size,PROBE_TEXT_END,PROBE_RO_END,fb);
#endif
    smp_init();timer_init();serial("AURORA: private CR3, W^X, IPC, PIT preemption ready\r\n");
    __atomic_store_n(&kernel_started,1,__ATOMIC_RELEASE);current_task=TASK_COUNT-1;enter_user(schedule());
}
