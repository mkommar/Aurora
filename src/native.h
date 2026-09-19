/* Limited Linux x86-64 ABI personality for the pinned static GCC/musl tools.
 * This is Aurora code: no Linux kernel or host-side compilation service runs. */
#include "rtc_time.h"
#include "virtio_block.h"
#include "virtio_net.h"
#include "native_fs.h"
#define NATIVE_SIZE 0x20000000ULL
#define NATIVE_MMAP_BASE 0x10000000ULL
/* Software page-table bits: COW and SHARED are hardware-ignored bits 9/10.
 * LAZY marks a committed demand-zero page without physical backing; NONE keeps
 * a lazy page inaccessible (PROT_NONE) until mprotect grants access. */
#define NATIVE_COW 512ULL
#define NATIVE_SHARED 1024ULL
#define NATIVE_LAZY 2048ULL
#define NATIVE_NONE (1ULL<<52)
#define NATIVE_END (USER_BASE+NATIVE_SIZE)
#define NATIVE_STACK (NATIVE_END-0x200000)
#define NATIVE_SLOTS (TASK_COUNT-APP_FIRST)
#define NATIVE_FDS 64
#define NATIVE_DESCRIPTIONS 1024
/* Descriptor flags are per fd; offsets and status flags belong to the shared
 * open description, including across fork and dup. Zero is never allocated. */
typedef struct {u64 offset;u32 flags,refs;} NativeDescription;
#define native_descriptions ((NativeDescription *)(KERNEL_STATE+0x50000))
typedef struct {int kind,index;u32 description,flags;} NativeFd;
static int native_description(u32 flags){
    for(int i=1;i<NATIVE_DESCRIPTIONS;i++)if(!native_descriptions[i].refs){
        native_descriptions[i]=(NativeDescription){.flags=flags&~0x80000U,.refs=1};return i;
    }
    return 0;
}
typedef struct {
    NativeFd *fd;u64 brk,min_brk,map_next;int parent,vfork_parent,reaped;
    char cwd[256],exe[256];u64 sigmask;u32 umask;int pgid,sid,stopped_signal,continued;
    u32 fd_owner,signal_owner,fs_owner,tgid;u64 clear_tid,robust_head;int thread;
    u64 restore_mask;int restore_mask_valid;
    u64 timer_deadline[3],timer_interval[3];char name[16];
} NativeProcess;
#define native_process ((NativeProcess *)(KERNEL_STATE+0x30000))
#define native_fd_tables ((NativeFd (*)[NATIVE_FDS])(KERNEL_STATE+0x20000))
_Static_assert(sizeof(NativeProcess)*TASK_COUNT<=0x10000,"process table exceeds its reservation");
_Static_assert(sizeof(NativeFd)*NATIVE_FDS*TASK_COUNT<=0x10000,"descriptor tables exceed their reservation");
_Static_assert(sizeof(NativeDescription)*NATIVE_DESCRIPTIONS<=0x10000,"open descriptions exceed their reservation");
static u16 native_fd_users[TASK_COUNT];
static u16 native_vm_refs[TASK_COUNT];
static u16 native_group_refs[TASK_COUNT];
static u32 native_vm_owner[TASK_COUNT];
static u8 native_vm_attached[TASK_COUNT];
static void native_wait_reset(u32 id);
static void native_thread_exit(u32 id);
static void native_mark_group(u32 id,i64 code);
static u8 native_reap[TASK_COUNT];
#include "vfs_operations.h"
typedef struct {u8 termios[36],input[4096];u32 size,ready,eof,foreground,root;} NativeTty;
#define NATIVE_TTY ((NativeTty *)0x0c200000)
typedef struct {u64 handler,flags,restorer,mask;} NativeSigaction;
/* Per-task signal state. Handler frames live on the user stack (Linux layout),
 * so nested delivery and sigreturn need no saved kernel copy. Dispositions are
 * shared through the signal owner; pending bits, senders and the alternate
 * stack belong to each task. */
typedef struct {NativeSigaction action[65];u64 pending;u32 sender[65];int code[65];u64 value[65];u64 fault_address,alt_sp,alt_size;} NativeSignals;
volatile u64 native_signal_deliveries,native_fault_signals;
static void native_notify_parent(u32 id,int code,int value);
static void native_stop_group(u32 id,u32 signal);
_Static_assert(sizeof(NativeSignals)<=4096,"signal state must fit reserved page");
static NativeSignals *native_signals(u32 id){return (NativeSignals *)(0x0c000000ULL+id*4096);}
static NativeSigaction *native_actions(u32 id){return native_signals(native_process[id].signal_owner)->action;}
#define NATIVE_PIPE_CAPACITY 4096
#define NATIVE_PIPES 64
typedef struct {u8 bytes[NATIVE_PIPE_CAPACITY];u32 size,readers,writers;} NativePipe;
#define native_pipes ((NativePipe *)0x0c300000)
_Static_assert(sizeof(NativePipe)*NATIVE_PIPES<=0x100000,"pipe buffers exceed their reservation");
#define NATIVE_EXEC_ARGS 4096
#define NATIVE_EXEC_BYTES 0x100000
#define exec_strings ((char *)0x0c400000)
#define exec_args ((char **)0x0c600000)
#define exec_argv ((u64 *)0x0c610000)
#define exec_env ((char (*)[4096])0x0c500000)
static int exec_argc,exec_envc;
/* Supervisor-only aliases mirror the process mappings. Physical pages come
 * from BIOS-reported RAM; no per-process contiguous reservation is needed.
 * Per-slot page-table roots and alias tables are indexed by task slot. */
#define NATIVE_PAGE_FIRST 0x10000000ULL
#define NATIVE_PAGE_LIMIT 0x40000000ULL
#define NATIVE_PAGE_COUNT ((NATIVE_PAGE_LIMIT-NATIVE_PAGE_FIRST)/4096)
#define NATIVE_PAGE_BITMAP ((u8 *)0x08800000)
#define NATIVE_PAGE_REFS ((u16 *)0x08810000)
#define NATIVE_ALIAS_PD ((u64 *)0x08700000)
#define NATIVE_ALIAS_TABLES 0x04400000ULL
_Static_assert(KERNEL_STATE+KERNEL_STATE_SIZE<=NATIVE_ALIAS_TABLES&&EXTENDED_STACKS_PHYSICAL+0x100000ULL<=NATIVE_ALIAS_TABLES,"kernel state overlaps the alias tables");
#define NATIVE_ROOTS 0x06400000ULL
#define NATIVE_ROOT_SIZE 0x110000ULL
_Static_assert(NATIVE_ALIAS_TABLES+TASK_COUNT*0x100000ULL<=NATIVE_ROOTS,"alias tables overlap page-table roots");
_Static_assert(NATIVE_ROOTS+TASK_COUNT*NATIVE_ROOT_SIZE<=0x08700000ULL,"page-table roots overlap the alias directory");
_Static_assert(TASK_COUNT*NATIVE_SIZE/0x40000000ULL<=16,"alias directory needs more than 16 pages");
static u32 native_free_pages,native_lazy_pages;
static u32 native_page_hint;
volatile u64 native_lazy_faults,native_lazy_commit_failures;
static u32 native_space(u32 id){return native_vm_attached[id]?native_vm_owner[id]:id;}
volatile u64 native_vm_shootdowns;
static void native_vm_barrier(u32 id){
    Cpu *self=cpu_local();if(!self->compat_owned)panic("VM mutation outside native state lock");
    u32 bit=1U<<native_space(id);if(self->vm_barriers&bit)return;self->vm_barriers|=bit;
    u64 root=task_cr3[id];int sent=0;
    for(u32 i=0;i<cpu_count;i++)if(i!=self->index&&cpus[i].online&&__atomic_load_n(&cpus[i].in_user,__ATOMIC_ACQUIRE)&&cpus[i].task<TASK_COUNT&&task_cr3[cpus[i].task]==root){cpu_ipi(cpus[i].apic_id,62);sent=1;}
    for(u32 i=0;i<cpu_count;i++)if(i!=self->index&&cpus[i].online)
        while(__atomic_load_n(&cpus[i].in_user,__ATOMIC_ACQUIRE)&&cpus[i].task<TASK_COUNT&&task_cr3[cpus[i].task]==root)__asm__ volatile("pause");
    native_vm_shootdowns+=sent;
}
static u64 native_phys(u32 id){return 0x100000000ULL+(u64)native_space(id)*NATIVE_SIZE;}
static u64 *native_pt(u32 id){return (u64 *)(task_cr3[id]+0x6000);}
static u64 *native_alias_pt(u32 id){return (u64 *)(NATIVE_ALIAS_TABLES+(u64)native_space(id)*0x100000ULL);}
/* A page belongs to a mapping when it is present or committed lazily. */
static int native_mapped(u64 alias){return (alias&(PRESENT|NATIVE_LAZY))!=0;}
/* Admission follows Linux's default heuristic overcommit: one request must fit
 * in free RAM, but outstanding lazy commitments are not summed, so forking a
 * process with a large untouched heap succeeds. A fault-in that finds no RAM
 * raises SIGSEGV in the faulting process (native_lazy_commit_failures). */
static u32 native_available_pages(void){return native_free_pages;}
static void native_memory_init(void){
    if(!native_ready)return;
    memset(native_pipes,0,NATIVE_PIPES*sizeof(NativePipe));
    memset(NATIVE_PAGE_BITMAP,0xff,NATIVE_PAGE_COUNT/8);memset(NATIVE_PAGE_REFS,0,NATIVE_PAGE_COUNT*2);memset(NATIVE_ALIAS_PD,0,65536);
    u32 count=*(u32 *)0x72000;if(count>64)count=0;
    for(u32 i=0;i<count;i++){
        u64 *entry=(u64 *)(0x72010ULL+i*24);if(*(u32 *)(entry+2)!=1)continue;
        u64 start=entry[0],length=entry[1];if(start>=NATIVE_PAGE_LIMIT||length>~0ULL-start)continue;
        u64 end=start+length;if(end>NATIVE_PAGE_LIMIT)end=NATIVE_PAGE_LIMIT;
        if(start<NATIVE_PAGE_FIRST)start=NATIVE_PAGE_FIRST;
        start=(start+4095)&~4095ULL;end&=~4095ULL;
        for(u64 address=start;address<end;address+=4096){u64 page=(address-NATIVE_PAGE_FIRST)/4096;
            if(NATIVE_PAGE_BITMAP[page/8]&(1U<<(page%8))){NATIVE_PAGE_BITMAP[page/8]&=~(1U<<(page%8));native_free_pages++;}}
    }
    for(int i=0;i<16;i++)((u64 *)0x201000)[4+i]=(0x08700000ULL+i*4096)|3;
    serial("NATIVE: BIOS page allocator free pages=");hex(native_free_pages);serial("\r\n");
}
static u64 native_page_allocate(void){
    if(!native_free_pages)return 0;
    for(u64 n=0;n<NATIVE_PAGE_COUNT/8;n++){u64 i=(native_page_hint+n)%(NATIVE_PAGE_COUNT/8);if(NATIVE_PAGE_BITMAP[i]!=255){
        for(u32 bit=0;bit<8;bit++)if(!(NATIVE_PAGE_BITMAP[i]&(1U<<bit))){
            NATIVE_PAGE_BITMAP[i]|=1U<<bit;NATIVE_PAGE_REFS[i*8+bit]=1;native_free_pages--;u64 address=NATIVE_PAGE_FIRST+(i*8+bit)*4096;
            native_page_hint=i;memset((void *)address,0,4096);return address;
        }
    }}
    return 0;
}
static void native_page_release(u64 address){
    if(address<NATIVE_PAGE_FIRST||address>=NATIVE_PAGE_LIMIT)return;
    u64 page=(address-NATIVE_PAGE_FIRST)/4096;
    if(NATIVE_PAGE_REFS[page]&&!--NATIVE_PAGE_REFS[page]){NATIVE_PAGE_BITMAP[page/8]&=~(1U<<(page%8));native_free_pages++;}
}
/* Give a committed lazy page its zeroed physical backing. Adding a present
 * translation needs no remote invalidation; a racing thread finds it present. */
static int native_fault_in(u32 id,u64 page){
    u64 *pt=native_pt(id),*alias=native_alias_pt(id);
    if(alias[page]&PRESENT)return 1;
    if(!(alias[page]&NATIVE_LAZY))return 0;
    u64 fresh=native_page_allocate();if(!fresh){native_lazy_commit_failures++;return 0;}
    if(native_lazy_pages)native_lazy_pages--;native_lazy_faults++;
    pt[page]=fresh|PRESENT|(pt[page]&(USER|WRITE|NX|NATIVE_SHARED));
    alias[page]=fresh|PRESENT|WRITE|NX;
    u64 address=native_phys(id)+page*4096;__asm__ volatile("invlpg (%0)"::"r"(address):"memory");return 1;
}
static int native_private_page(u32 id,u64 page){
    native_vm_barrier(id);
    u64 *pt=native_pt(id),*alias=native_alias_pt(id),old=alias[page]&0x000ffffffffff000ULL;
    if(!(alias[page]&1))return 0;
    if(NATIVE_PAGE_REFS[(old-NATIVE_PAGE_FIRST)/4096]>1){
        u64 fresh=native_page_allocate();if(!fresh)return 0;
        memcpy((void *)fresh,(void *)old,4096);native_page_release(old);
        pt[page]=(pt[page]&~0x000ffffffffff000ULL)|fresh;alias[page]=fresh|PRESENT|WRITE|NX;
    }
    if(pt[page]&NATIVE_COW)pt[page]=(pt[page]&~NATIVE_COW)|WRITE;
    u64 address=native_phys(id)+page*4096;__asm__ volatile("invlpg (%0)"::"r"(address):"memory");return 1;
}
static int native_write_fault(u32 id,u64 address,u64 error){
    if(!native_active[id]||(error&7)!=7||address<USER_BASE||address>=NATIVE_END)return 0;
    u64 page=(address-USER_BASE)/4096;if(!(native_pt(id)[page]&NATIVE_COW))return 0;
    return native_private_page(id,page);
}
/* Not-present user faults on committed lazy pages allocate zero-filled RAM.
 * Access kinds the mapping does not permit fall through to fault handling. */
static int native_lazy_fault(u32 id,u64 address,u64 error){
    if(!native_active[id]||(error&5)!=4||address<USER_BASE||address>=NATIVE_END)return 0;
    u64 page=(address-USER_BASE)/4096,pte=native_pt(id)[page];
    if(!(pte&NATIVE_LAZY)||(pte&NATIVE_NONE))return 0;
    if((error&2)&&!(pte&WRITE))return 0;
    if((error&16)&&(pte&NX))return 0;
    return native_fault_in(id,page);
}
static void native_unmap_page(u32 id,u64 page){
    native_vm_barrier(id);
    u64 *pt=native_pt(id),*alias=native_alias_pt(id);
    if(alias[page]&1)native_page_release(alias[page]&0x000ffffffffff000ULL);
    else if((alias[page]&NATIVE_LAZY)&&native_lazy_pages)native_lazy_pages--;
    pt[page]=0;alias[page]=0;
    u64 address=native_phys(id)+page*4096;__asm__ volatile("invlpg (%0)"::"r"(address):"memory");
}
static void native_memory_release(u32 id){
    if(!native_vm_attached[id])return;u32 owner=native_vm_owner[id];
    if(!--native_vm_refs[owner])for(u64 i=0;i<NATIVE_SIZE/4096;i++)if(native_mapped(native_alias_pt(id)[i]))native_unmap_page(id,i);
    native_vm_attached[id]=0;
}
/* Kernel access to user memory resolves lazy and copy-on-write pages first. */
static void *native_buffer(u32 id,u64 address,u64 size,int write){
    if(!size||address<USER_BASE||address>=NATIVE_END||size>NATIVE_END-address)return 0;
    u64 *pt=native_pt(id);
    for(u64 p=(address-USER_BASE)/4096;p<=(address+size-1-USER_BASE)/4096;p++){
        u64 pte=pt[p];int accessible=(pte&5)==5||((pte&(NATIVE_LAZY|USER))==(NATIVE_LAZY|USER)&&!(pte&NATIVE_NONE));
        if(!accessible||(write&&!(pte&(WRITE|NATIVE_COW))))return 0;
        if((pte&NATIVE_LAZY)&&!native_fault_in(id,p))return 0;
        if(write&&(pt[p]&NATIVE_COW)&&!native_private_page(id,p))return 0;
    }
    return (void *)(native_phys(id)+address-USER_BASE);
}
static int native_string(u64 address,char *out,u64 capacity){
    for(u64 i=0;i<capacity;i++){char *p=native_buffer(current_task,address+i,1,0);if(!p)return 0;out[i]=*p;if(!out[i])return 1;}return 0;
}
static void native_tables(u32 id){
    native_memory_release(id);
    native_vm_owner[id]=id;native_vm_refs[id]=1;native_vm_attached[id]=1;
    u64 root=NATIVE_ROOTS+id*NATIVE_ROOT_SIZE;task_cr3[id]=root;memset((void *)root,0,NATIVE_ROOT_SIZE);
    u64 *pml4=(u64 *)root,*pdpt=(u64 *)(root+0x1000),*pd=(u64 *)(root+0x2000);
    pml4[0]=(root+0x1000)|7;for(int i=0;i<4;i++)pdpt[i]=(root+0x2000+i*4096)|7;
    for(int i=0;i<2048;i++)pd[i]=(u64)i*0x200000|PRESENT|WRITE|HUGE;
    pd[0]=0x207003;pd[1]=0x208003;pd[EXTENDED_STACKS/0x200000]=EXTENDED_STACK_TABLE|3;
    memset(native_alias_pt(id),0,0x100000);
    for(int i=0;i<256;i++){pd[i+2]=(root+0x6000+i*4096)|7;NATIVE_ALIAS_PD[id*256+i]=((u64)native_alias_pt(id)+i*4096)|3;}
    native_active[id]=1;
}
/* Map a range. Present pages keep their identity after becoming private;
 * new pages are either allocated now or committed lazily. A request larger
 * than free RAM fails with ENOMEM up front (heuristic overcommit, see
 * native_available_pages). Shared mappings are always backed eagerly. */
static int native_map_pages(u32 id,u64 address,u64 size,u64 flags,int lazy){
    native_vm_barrier(id);
    if(!size||address<USER_BASE||address>=NATIVE_END||size>NATIVE_END-address)return 0;
    if(flags&NATIVE_SHARED)lazy=0;
    u64 *pt=native_pt(id),*alias=native_alias_pt(id);
    u64 needed=0;for(u64 page=(address-USER_BASE)/4096;page<=(address+size-1-USER_BASE)/4096;page++){
        if(!(alias[page]&1)){if(!(alias[page]&NATIVE_LAZY))needed++;}
        else if(NATIVE_PAGE_REFS[((alias[page]&0x000ffffffffff000ULL)-NATIVE_PAGE_FIRST)/4096]>1)needed++;}
    if(needed>native_available_pages())return 0;
    for(u64 page=(address-USER_BASE)/4096;page<=(address+size-1-USER_BASE)/4096;page++){
        if(alias[page]&1){if(!native_private_page(id,page))return 0;
            pt[page]=(alias[page]&0x000ffffffffff000ULL)|PRESENT|USER|flags;}
        else if(lazy){if(!(alias[page]&NATIVE_LAZY))native_lazy_pages++;pt[page]=NATIVE_LAZY|USER|flags;alias[page]=NATIVE_LAZY;}
        else{u64 physical=native_page_allocate();if(!physical)return 0;
            if((alias[page]&NATIVE_LAZY)&&native_lazy_pages)native_lazy_pages--;
            pt[page]=physical|PRESENT|USER|flags;alias[page]=physical|PRESENT|WRITE|NX;}
        u64 kernel_alias=native_phys(id)+page*4096;__asm__ volatile("invlpg (%0)"::"r"(kernel_alias):"memory");
    }return 1;
}
static int native_map(u32 id,u64 address,u64 size,u64 flags){return native_map_pages(id,address,size,flags,0);}
/* Search mapped pages instead of consuming the mmap arena monotonically. */
static u64 native_mapping_gap(u32 id,u64 size){
    u64 run=0,*pt=native_alias_pt(id);
    for(u64 address=NATIVE_MMAP_BASE;address<NATIVE_STACK-4096;address+=4096){
        if(native_mapped(pt[(address-USER_BASE)/4096]))run=0;else run+=4096;
        if(run>=size)return address+4096-size;
    }
    return 0;
}
static int native_range_free(u32 id,u64 address,u64 size){
    if(address<USER_BASE||address>=NATIVE_STACK-4096||size>NATIVE_STACK-4096-address)return 0;
    u64 *alias=native_alias_pt(id);
    for(u64 page=(address-USER_BASE)/4096;page<(address+size-USER_BASE)/4096;page++)if(native_mapped(alias[page]))return 0;
    return 1;
}
/* mremap: shrink in place, grow in place when the following range is free,
 * otherwise relocate translations (MREMAP_MAYMOVE) without copying page data. */
static i64 native_mremap(u32 id,u64 old_address,u64 old_size,u64 new_size,u64 flags,u64 new_address){
    if((old_address&4095)||!old_size||!new_size||(flags&~3ULL)||((flags&2)&&!(flags&1)))return -22;
    old_size=(old_size+4095)&~4095ULL;new_size=(new_size+4095)&~4095ULL;
    if(old_address<USER_BASE||old_address>=NATIVE_END||old_size>NATIVE_END-old_address||new_size>NATIVE_SIZE)return -22;
    u64 *pt=native_pt(id),*alias=native_alias_pt(id),first=(old_address-USER_BASE)/4096;
    for(u64 page=first;page<first+old_size/4096;page++)if(!native_mapped(alias[page]))return -14;
    u64 attributes=pt[first]&(WRITE|NX|NATIVE_SHARED|NATIVE_NONE);
    if(new_size<=old_size){
        for(u64 page=first+new_size/4096;page<first+old_size/4096;page++)native_unmap_page(id,page);
        return old_address;
    }
    if(!(flags&2)&&native_range_free(id,old_address+old_size,new_size-old_size)){
        if(!native_map_pages(id,old_address+old_size,new_size-old_size,attributes&(WRITE|NX|NATIVE_SHARED),1))return -12;
        return old_address;
    }
    if(!(flags&1))return -12;
    u64 target;
    if(flags&2){target=new_address;if((target&4095)||target<USER_BASE||target>=NATIVE_STACK-4096||new_size>NATIVE_STACK-4096-target)return -22;
        if(target<old_address+old_size&&old_address<target+new_size)return -22;
        for(u64 page=(target-USER_BASE)/4096;page<(target+new_size-USER_BASE)/4096;page++)if(native_mapped(alias[page]))native_unmap_page(id,page);
    }else{target=native_mapping_gap(id,new_size);if(!target)return -12;}
    if(new_size-old_size>(u64)native_available_pages()*4096)return -12;
    native_vm_barrier(id);
    u64 destination=(target-USER_BASE)/4096;
    for(u64 i=0;i<old_size/4096;i++){
        pt[destination+i]=pt[first+i];alias[destination+i]=alias[first+i];pt[first+i]=0;alias[first+i]=0;
        u64 from=native_phys(id)+(first+i)*4096,to=native_phys(id)+(destination+i)*4096;
        __asm__ volatile("invlpg (%0); invlpg (%1)"::"r"(from),"r"(to):"memory");
    }
    if(!native_map_pages(id,target+old_size,new_size-old_size,attributes&(WRITE|NX|NATIVE_SHARED),1))return -12;
    return target;
}
static void native_close(u32 id,int fd){
    NativeFd *f=&native_process[id].fd[fd];
    int file_index=f->kind==1?f->index:-1;
    if(f->description&&native_descriptions[f->description].refs)native_descriptions[f->description].refs--;
    if(f->kind==6&&!native_descriptions[f->description].refs)network_close(f->index);
    if(f->kind==2&&native_pipes[f->index].readers)native_pipes[f->index].readers--;
    if(f->kind==3&&native_pipes[f->index].writers)native_pipes[f->index].writers--;
    memset(f,0,sizeof(*f));
    if(file_index>=0)vfs_close_deleted(file_index);
}
/* Post a signal with siginfo details. SIGKILL and SIGSTOP act immediately;
 * SIGCONT resumes a stopped group and reports it to the parent. Others become
 * pending and are delivered before the task next returns to ring 3. */
static void native_signal_post(u32 id,u32 signal,u32 sender,int code,u64 value){
    if(!signal||signal>64||!native_active[id])return;NativeSignals *s=native_signals(id);
    s->sender[signal]=sender;s->code[signal]=code;s->value[signal]=value;
    if(task_kernel_sp[id]){s->pending|=1ULL<<(signal-1);return;}
    if(signal==9){native_mark_group(id,-9);return;}
    if(signal==19){native_stop_group(id,19);return;}
    if(signal==18){u32 group=native_process[id].tgid;int resumed=0;
        for(u32 member=APP_FIRST;member<TASK_COUNT;member++)if(native_active[member]&&native_process[member].tgid==group&&tasks[member].state==STOPPED){tasks[member].state=RUNNABLE;resumed=1;}
        if(resumed){native_process[id].continued=1;native_process[id].stopped_signal=0;native_notify_parent(id,6,18);}}
    s->pending|=1ULL<<(signal-1);
}
static void native_signal_queue(u32 id,u32 signal){native_signal_post(id,signal,0,0x80,0);}
/* SIGCHLD to the parent with CLD_* code and the child's status value. */
static void native_notify_parent(u32 id,int code,int value){
    int parent=native_process[id].parent;if(parent<0||parent>=(int)TASK_COUNT||!native_active[parent])return;
    native_signal_post(parent,17,native_process[id].tgid,code,(u64)(u32)value);
}
static void native_stop_group(u32 id,u32 signal){
    u32 group=native_process[id].tgid;
    for(u32 member=APP_FIRST;member<TASK_COUNT;member++)if(native_active[member]&&native_process[member].tgid==group&&(tasks[member].state==RUNNABLE||tasks[member].state==WAIT_EVENT)){
        tasks[member].state=STOPPED;if(task_cpu[member]>=0&&(u32)task_cpu[member]!=cpu_local()->index)cpu_ipi(cpus[task_cpu[member]].apic_id,62);}
    native_process[id].stopped_signal=signal;native_process[id].continued=0;native_notify_parent(id,5,signal);
}
/* Interval timers fire SIGALRM/SIGVTALRM/SIGPROF at tick granularity. Virtual
 * and profiling timers advance with wall time; Aurora keeps no finer CPU
 * accounting than the scheduler tick. Timers belong to the thread-group leader. */
static void native_timers(void){
    for(u32 id=APP_FIRST;id<TASK_COUNT;id++){if(!native_active[id]||tasks[id].state==DEAD||native_process[id].thread)continue;NativeProcess *p=&native_process[id];
        for(int t=0;t<3;t++)if(p->timer_deadline[t]&&timer_ticks>=p->timer_deadline[t]){
            p->timer_deadline[t]=p->timer_interval[t]?timer_ticks+p->timer_interval[t]:0;
            native_signal_post(id,t==0?14:t==1?26:27,0,-2,0);}}
}
static void native_finish(u32 id,i64 code){
    native_wait_reset(id);
    NativeProcess *p=&native_process[id];native_thread_exit(id);
    if(p->fd&&native_fd_users[p->fd_owner]&&!--native_fd_users[p->fd_owner])for(int i=0;i<NATIVE_FDS;i++)native_close(id,i);
    native_memory_release(id);
    task_kernel_sp[id]=0;if(p->thread)p->reaped=1;
    tasks[id].state=DEAD;exit_codes[id]=code;
    u32 leader=p->tgid>=100?p->tgid-100:id;
    if(leader<TASK_COUNT&&native_group_refs[leader]&&!--native_group_refs[leader]){
        exit_codes[leader]=code;native_process[leader].timer_deadline[0]=native_process[leader].timer_deadline[1]=native_process[leader].timer_deadline[2]=0;
        native_notify_parent(leader,code<0?2:1,code<0?(code<=-128?11:(int)-code):(int)code&255);}
    if(leader<TASK_COUNT&&!native_group_refs[leader])for(u32 child=APP_FIRST;child<TASK_COUNT;child++)if(native_active[child]&&native_process[child].parent==(int)leader)native_process[child].parent=-1;
    if(p->vfork_parent>=0){tasks[p->vfork_parent].state=RUNNABLE;p->vfork_parent=-1;}
}
static int native_on_alt(NativeSignals *s,u64 rsp){return s->alt_size&&rsp>s->alt_sp&&rsp-s->alt_sp<=s->alt_size;}
/* Deliver one pending unblocked signal. Returns 1 when the task's frame or
 * state changed. Handler frames follow the Linux rt_sigframe layout: return
 * address, siginfo, ucontext with mcontext/uc_sigmask and FXSAVE state. The
 * mask restored by sigreturn comes from the frame, so a mask saved by
 * sigsuspend/pselect is reinstated after the handler rather than before it.
 * Frames stack on the user stack, so handlers may nest. */
static int native_signal_deliver(u32 id){
    if(!native_active[id])return 0;NativeSignals *s=native_signals(id);NativeProcess *p=&native_process[id];Frame *f=&tasks[id].frame;
    u64 pending=s->pending&~p->sigmask;if(!pending){if(p->restore_mask_valid){p->sigmask=p->restore_mask;p->restore_mask_valid=0;}return 0;}
    u32 signal=1;while(!(pending&1)){signal++;pending>>=1;}s->pending&=~(1ULL<<(signal-1));
    NativeSigaction action=native_actions(id)[signal];if(action.handler==1)return 1;
    if(!action.handler){if(signal==17||signal==18||signal==23||signal==28)return 1;
        if(signal==20||signal==21||signal==22){native_stop_group(id,signal);return 1;}
        native_mark_group(id,-(i64)signal);return 1;}
    if(!action.restorer||!native_buffer(id,action.handler,1,0)||!native_buffer(id,action.restorer,1,0)){native_mark_group(id,-11);return 1;}
    u64 top=f->rsp;
    if((action.flags&0x08000000)&&s->alt_size&&!native_on_alt(s,f->rsp))top=s->alt_sp+s->alt_size;
    if(top<USER_BASE+1672){native_mark_group(id,-11);return 1;}
    u64 base=(top-128-1536)&~15ULL,sp=base-8;
    u8 *buffer=native_buffer(id,sp,1544,1);if(!buffer){native_mark_group(id,-11);return 1;}
    u64 frame_mask=p->restore_mask_valid?p->restore_mask:p->sigmask;p->restore_mask_valid=0;
    memset(buffer,0,1544);*(u64 *)buffer=action.restorer;
    int code=s->code[signal];
    *(u32 *)(buffer+8)=signal;*(int *)(buffer+16)=code;
    if(code>0&&(signal==11||signal==7||signal==8||signal==4))*(u64 *)(buffer+24)=s->fault_address;
    else{*(u32 *)(buffer+24)=s->sender[signal];*(u32 *)(buffer+28)=1000;*(u64 *)(buffer+32)=s->value[signal];}
    u64 *context=(u64 *)(buffer+8+128+40);
    u64 regs[]={f->r8,f->r9,f->r10,f->r11,f->r12,f->r13,f->r14,f->r15,f->rdi,f->rsi,f->rbp,f->rbx,f->rdx,f->rax,f->rcx,f->rsp,f->rip,f->flags,0x001b000000000023ULL,0,0,frame_mask,s->fault_address,base+512};
    memcpy(context,regs,sizeof(regs));*(u64 *)(buffer+8+128+296)=frame_mask;
    *(u64 *)(buffer+8+128+16)=s->alt_sp;*(u64 *)(buffer+8+128+24)=native_on_alt(s,f->rsp)?1:s->alt_size?0:2;*(u64 *)(buffer+8+128+32)=s->alt_size;
    memcpy(buffer+8+512,task_fp[id],512);
    p->sigmask|=action.mask;if(!(action.flags&0x40000000))p->sigmask|=1ULL<<(signal-1);p->sigmask&=~((1ULL<<8)|(1ULL<<18));
    if(action.flags&0x80000000)native_actions(id)[signal].handler=0;
    f->rsp=sp;f->rip=action.handler;f->rdi=signal;f->rsi=base;f->rdx=base+128;f->rax=0;native_signal_deliveries++;return 1;
}
/* rt_sigreturn: restore the frame that the handler returned through. Segment
 * selectors and privileged flag bits are fixed; the FXSAVE image is sanitized. */
static int native_sigreturn(u32 id){
    NativeProcess *p=&native_process[id];Frame *f=&tasks[id].frame;u64 base=f->rsp;
    u64 *g=native_buffer(id,base+128+40,24*8,0),*mask=native_buffer(id,base+128+296,8,0);
    if(!g||!mask||g[16]>=NATIVE_END)return 0;
    f->r8=g[0];f->r9=g[1];f->r10=g[2];f->r11=g[3];f->r12=g[4];f->r13=g[5];f->r14=g[6];f->r15=g[7];f->rdi=g[8];f->rsi=g[9];f->rbp=g[10];f->rbx=g[11];
    f->rdx=g[12];f->rax=g[13];f->rcx=g[14];f->rsp=g[15];f->rip=g[16];f->flags=(g[17]&0xcd5)|0x202;f->cs=0x23;f->ss=0x1b;
    if(g[23]){u8 *fp=native_buffer(id,g[23],512,0);if(!fp)return 0;memcpy(task_fp[id],fp,512);*(u32 *)(task_fp[id]+24)&=0xffff;}
    p->sigmask=*mask&~((1ULL<<8)|(1ULL<<18));return 1;
}
/* Faults in native tasks become catchable signals when a handler is installed
 * and the signal is not blocked; otherwise the existing containment kills. */
static int native_fault_signal(u32 id,u64 vector,u64 address){
    if(!native_active[id])return 0;
    u32 signal=vector==0||vector==16||vector==19?8:vector==6?4:vector==17||vector==12?7:11;
    NativeSigaction *action=&native_actions(id)[signal];
    if(action->handler<2||(native_process[id].sigmask&(1ULL<<(signal-1))))return 0;
    native_signals(id)->fault_address=address;
    native_signal_post(id,signal,0,1,0);native_fault_signals++;return 1;
}
static int application_slot_available(u32 i){return tasks[i].state==DEAD&&task_cpu[i]<0&&!native_vm_refs[i]&&!native_fd_users[i]&&!native_group_refs[i]&&(!native_active[i]||native_process[i].reaped||native_process[i].parent<0);}
static int native_slot(void){for(int i=APP_FIRST;i<APP_FIRST+NATIVE_SLOTS;i++)if(application_slot_available(i))return i;return -1;}
static void native_defaults(u32 id){
    task_affinity[id]=(1U<<cpu_count)-1;
    native_wait_reset(id);
    memset(&native_process[id],0,sizeof(NativeProcess));NativeProcess *p=&native_process[id];
    p->fd=native_fd_tables[id];memset(p->fd,0,sizeof(native_fd_tables[id]));p->fd_owner=p->signal_owner=p->fs_owner=id;p->tgid=id+100;native_fd_users[id]=native_group_refs[id]=1;
    p->parent=-1;p->vfork_parent=-1;p->umask=022;ns_copy(p->cwd,"/work");
    p->pgid=p->sid=id+100;memset(native_signals(id),0,sizeof(NativeSignals));
    memset(NATIVE_TTY,0,sizeof(NativeTty));NATIVE_TTY->foreground=id+100;NATIVE_TTY->root=id;
    *(u32 *)NATIVE_TTY->termios=0x100;*(u32 *)(NATIVE_TTY->termios+4)=5;*(u32 *)(NATIVE_TTY->termios+8)=0xbf;*(u32 *)(NATIVE_TTY->termios+12)=0x3b;
    NATIVE_TTY->termios[17]=3;NATIVE_TTY->termios[18]=28;NATIVE_TTY->termios[19]=127;NATIVE_TTY->termios[20]=21;NATIVE_TTY->termios[21]=4;NATIVE_TTY->termios[23]=1;NATIVE_TTY->termios[27]=26;
    for(int i=0;i<3;i++)p->fd[i]=(NativeFd){.kind=4,.index=i,.description=native_description(i?1:0)};
}
#include "native_elf.h"
static i64 native_exec(u32 id,int index){
    if(native_vm_attached[id]&&native_vm_refs[native_space(id)]>1&&native_process[id].vfork_parent<0)return -16;
    if(*(u32 *)(NFILES[index].pad+4)&&!(*(u32 *)NFILES[index].pad&0100))return -13;
    NativeElf main,interpreter;char interp[256];
    i64 error=native_elf_read(index,USER_BASE,0x4000000,&main,interp);
    if(error)return error;
    int loader=-1;
    if(interp[0]){
        loader=native_find(interp);if(loader<0)return -2;
        if(*(u32 *)(NFILES[loader].pad+4)&&!(*(u32 *)NFILES[loader].pad&0100))return -13;
        char nested[256];error=native_elf_read(loader,0x08000000,0x0c000000,&interpreter,nested);
        if(error)return error;if(nested[0]||interpreter.header.type!=3)return -8;
    }
    u64 needed=512+main.pages+(loader>=0?interpreter.pages:0),available=native_available_pages();
    if(native_vm_attached[id]&&native_vm_refs[native_space(id)]==1)for(u64 i=0;i<NATIVE_SIZE/4096;i++)if((native_alias_pt(id)[i]&1)&&NATIVE_PAGE_REFS[((native_alias_pt(id)[i]&0x000ffffffffff000ULL)-NATIVE_PAGE_FIRST)/4096]==1)available++;
    if(needed>available)return -12;
    native_tables(id);
    error=native_elf_map(id,index,&main);
    if(!error&&loader>=0)error=native_elf_map(id,loader,&interpreter);
    if(error){native_finish(id,127);return error;}
    /* The 2 MiB stack is committed lazily; only the top 256 KiB holding the
       initial arguments, environment and auxiliary vector is backed now. */
    if(!native_map_pages(id,NATIVE_STACK,0x200000,WRITE|NX,1)||!native_map(id,NATIVE_END-0x40000,0x40000,WRITE|NX)){native_finish(id,127);return -12;}
    u64 *argv=exec_argv,env[128],sp=NATIVE_END-32;
    for(int i=exec_envc-1;i>=0;i--){u64 n=ns_length(exec_env[i])+1;sp-=n;memcpy((void *)(native_phys(id)+sp-USER_BASE),exec_env[i],n);env[i]=sp;}
    for(int i=exec_argc-1;i>=0;i--){u64 n=ns_length(exec_args[i])+1;sp-=n;memcpy((void *)(native_phys(id)+sp-USER_BASE),exec_args[i],n);argv[i]=sp;}
    sp-=16;u64 random_address=sp;void *random=(void *)(native_phys(id)+sp-USER_BASE);memset(random,0,16);if(entropy_ready)entropy_fill(random,16);
    u64 aux[]={3,main.phaddr,4,sizeof(ElfSegment),5,main.header.phnum,6,4096,7,loader>=0?interpreter.bias:0,9,main.entry,11,1000,12,1000,13,1000,14,1000,23,0,25,random_address,31,exec_argc?argv[0]:0,0,0};
    u64 words=1+exec_argc+1+exec_envc+1+sizeof(aux)/8;sp=(sp-words*8)&~15ULL;
    u64 *stack=(u64 *)(native_phys(id)+sp-USER_BASE);*stack++=exec_argc;
    for(int i=0;i<exec_argc;i++)*stack++=argv[i];*stack++=0;
    for(int i=0;i<exec_envc;i++)*stack++=env[i];*stack++=0;memcpy(stack,aux,sizeof(aux));
    NativeProcess *p=&native_process[id];p->min_brk=p->brk=(main.end+4095)&~4095ULL;p->map_next=NATIVE_MMAP_BASE;p->clear_tid=p->robust_head=0;
    NativeSignals *signals=native_signals(id);memcpy(signals->action,native_actions(id),sizeof(signals->action));p->signal_owner=id;signals->alt_sp=signals->alt_size=0;p->restore_mask_valid=0;
    p->timer_deadline[0]=p->timer_deadline[1]=p->timer_deadline[2]=p->timer_interval[0]=p->timer_interval[1]=p->timer_interval[2]=0;
    {const char *base=NFILES[index].path,*slash=base;while(*slash){if(*slash=='/')base=slash+1;slash++;}int i=0;while(base[i]&&i<15){p->name[i]=base[i];i++;}p->name[i]=0;}
    for(int i=1;i<65;i++)if(signals->action[i].handler!=1)memset(&signals->action[i],0,sizeof(NativeSigaction));
    ns_copy(p->exe,NFILES[index].path);
    for(int i=0;i<NATIVE_FDS;i++)if(p->fd[i].flags&0x80000)native_close(id,i);
    if(p->vfork_parent>=0){tasks[p->vfork_parent].state=RUNNABLE;p->vfork_parent=-1;}
    memset(&tasks[id],0,sizeof(Task));tasks[id].frame=(Frame){.rip=loader>=0?interpreter.entry:main.entry,.rsp=sp,.cs=0x23,.ss=0x1b,.flags=0x202};
    task_fsbase[id]=task_gsbase[id]=0;memcpy(task_fp[id],initial_fp,512);task_faults[id]=0;exit_codes[id]=0;
    serial("NATIVE EXEC: ");serial(p->exe);serial("\r\n");return 0;
}
static i64 native_spawn(u64 address){
    SpawnRequest *r=user_buffer(current_task,address,sizeof(SpawnRequest),0);if(!r)return ERR_POINTER;
    if(!native_ready)return ERR_NOT_FOUND;if(!name_valid(r->name)||r->args[127])return ERR_NAME;
    char path[256];native_path(path,"/work",r->name);int index=native_find(path);
    if(index<0&&ext2_ready){native_path(path,"/usr/local/bin",r->name);index=native_find(path);}
    if(index<0&&ext2_ready){native_path(path,"/usr/bin",r->name);index=native_find(path);}
    if(index<0){native_path(path,"/bin",r->name);index=native_find(path);}
    if(index<0)return ERR_NOT_FOUND;
    int id=native_slot();if(id<0)return ERR_LIMIT;
    exec_argc=0;const char *s=r->args;for(int i=0;i<256;i++)exec_args[i]=exec_strings+i*4096;
    while(*s&&exec_argc<256){while(*s==' ')s++;if(!*s)break;int n=0;while(*s&&*s!=' '&&n<4095)exec_args[exec_argc][n++]=*s++;exec_args[exec_argc++][n]=0;}
    ns_copy(exec_args[0],path);
    exec_envc=5;ns_copy(exec_env[0],"PATH=/usr/local/bin:/usr/bin:/bin");ns_copy(exec_env[1],"TMPDIR=/tmp");ns_copy(exec_env[2],"LC_ALL=C");ns_copy(exec_env[3],"HOME=/work");ns_copy(exec_env[4],"TERM=dumb");
    native_defaults(id);i64 result=native_exec(id,index);if(result<0)native_finish(id,127);return result<0?ERR_FORMAT:id;
}
/* Legacy names map to /work on the development volume. */
static int native_shell_exists(const char *name){char path[256];return native_ready&&native_path(path,"/work",name)&&native_find(path)>=0;}
static i64 native_shell_executable(const char *name,void *buffer,u64 capacity){
    char path[256];if(!native_ready||!native_path(path,"/work",name))return ERR_NOT_FOUND;int index=native_find(path);
    if(index<0||NFILES[index].kind!=1)return ERR_NOT_FOUND;if(NFILES[index].size>capacity)return ERR_LIMIT;return native_read(index,0,buffer,NFILES[index].size);
}
static i64 native_shell_file(u64 address,int write){
    FileRequest *r=user_buffer(current_task,address,sizeof(FileRequest),0);if(!r)return ERR_POINTER;
    if(!native_ready)return ERR_NOT_FOUND;if(!name_valid(r->name)||r->size>USER_SIZE)return ERR_NAME;
    void *buffer=r->size?user_buffer(current_task,r->buffer,r->size,!write):0;if(r->size&&!buffer)return ERR_POINTER;
    char path[256];native_path(path,"/work",r->name);int index=native_find(path);
    if(index<0&&write)index=native_create(path);if(index<0)return ERR_NOT_FOUND;
    if(write){i64 n=native_write(index,0,buffer,r->size);if(n<0)return n;NFILES[index].size=r->size;return native_commit(index)?n:ERR_IO;} /* replace, truncating any previous tail */
    return native_read(index,0,buffer,r->size);
}
static i64 native_shell_list(u64 slot,u64 address){
    FileEntry *out=user_buffer(current_task,address,sizeof(FileEntry),1);if(!out)return ERR_POINTER;
    if(!native_ready)return ERR_NOT_FOUND;
    if(ext2_ready){
        ext4_dir dir;if(ext4_dir_open(&dir,"/work"))return ERR_NOT_FOUND;
        const ext4_direntry *entry;u64 n=0;
        while((entry=ext4_dir_entry_next(&dir))){
            if(entry->inode_type!=1)continue;
            if(n++!=slot)continue;
            char path[256]="/work/";u32 length=entry->name_length;if(length>249)continue;
            memcpy(path+6,entry->name,length);path[6+length]=0;int index=native_find(path);
            memset(out,0,sizeof(*out));if(length>31)length=31;memcpy(out->name,entry->name,length);
            out->size=index>=0?NFILES[index].size:0;out->used=1;ext4_dir_close(&dir);return 0;
        }
        ext4_dir_close(&dir);return ERR_EMPTY;
    }
    for(u32 i=0,n=0;i<native_count;i++)if(NFILES[i].kind==1&&native_writable(NFILES[i].path)&&NFILES[i].path[1]=='w'){
        if(n++==slot){memset(out,0,sizeof(*out));u64 length=ns_length(NFILES[i].path+6);if(length>31)length=31;memcpy(out->name,NFILES[i].path+6,length);out->size=NFILES[i].size;out->used=1;return 0;}
    }
    return ERR_EMPTY;
}
static int native_user_path(u64 address,char *out){char input[256];return native_string(address,input,sizeof(input))&&native_path(out,native_process[current_task].cwd,input);}
static int native_at_path(i64 fd,u64 address,char *out){
    fd=(int)fd; /* Linux dirfd arguments are signed 32-bit, including raw syscall(). */
    char input[256];if(!native_string(address,input,sizeof(input)))return -14;
    const char *cwd=native_process[current_task].cwd;
    if(input[0]!='/'&&fd!=-100){if(fd<0||fd>=NATIVE_FDS)return -9;NativeFd *f=&native_process[current_task].fd[fd];
        if(f->kind!=1)return -9;if(NFILES[f->index].kind!=2)return -20;cwd=NFILES[f->index].path;}
    return native_path(out,cwd,input)?0:-36;
}
static i64 native_getdents(u64 fd,u64 address,u64 size){
    NativeProcess *p=&native_process[current_task];if(fd>=NATIVE_FDS||p->fd[fd].kind!=1)return -9;
    NativeFile *f=&NFILES[p->fd[fd].index];if(f->kind!=2)return -20;
    u8 *out=native_buffer(current_task,address,size,1);if(!out)return -14;
    if(aurorafs_path(f->path)){NativeDescription *of=&native_descriptions[p->fd[fd].description];u64 done=0;
        for(u32 slot=(u32)of->offset;slot<FS_FILES;slot++){if(!directory[slot].used){of->offset=slot+1;continue;}
            u64 length=(20+ns_length(directory[slot].name)+7)&~7ULL;if(length>size-done){if(!done)return -22;break;}
            memset(out+done,0,length);*(u64 *)(out+done)=0x41000000+slot;*(u64 *)(out+done+8)=slot+1;*(u16 *)(out+done+16)=length;out[done+18]=8;
            memcpy(out+done+19,directory[slot].name,ns_length(directory[slot].name));of->offset=slot+1;done+=length;}
        return done;
    }
    if(fat_ready&&fat_path(f->path)){
        DIR dir;FRESULT error=f_opendir(&dir,fat_name(f->path));if(error)return fat_error(error);
        NativeDescription *of=&native_descriptions[p->fd[fd].description];u64 position=0,done=0;FILINFO info;
        while((error=f_readdir(&dir,&info))==FR_OK&&info.fname[0]){
            if(position++<of->offset)continue;u64 length=(20+ns_length(info.fname)+7)&~7ULL;
            if(length>size-done){if(!done){f_closedir(&dir);return -22;}break;}
            memset(out+done,0,length);*(u64 *)(out+done)=position;*(u64 *)(out+done+8)=position;*(u16 *)(out+done+16)=length;
            out[done+18]=(info.fattrib&AM_DIR)?4:8;memcpy(out+done+19,info.fname,ns_length(info.fname));of->offset=position;done+=length;
        }
        f_closedir(&dir);return error?fat_error(error):(i64)done;
    }
    if(!ext2_ready)return -38;
    ext4_dir dir;int error=ext4_dir_open(&dir,f->path);if(error)return -error;
    NativeDescription *of=&native_descriptions[p->fd[fd].description];dir.next_off=of->offset;u64 done=0;
    const ext4_direntry *entry;
    while((entry=ext4_dir_entry_next(&dir))){u64 length=(19+entry->name_length+1+7)&~7ULL;
        if(length>size-done){if(!done){ext4_dir_close(&dir);return -22;}break;}
        memset(out+done,0,length);*(u64 *)(out+done)=entry->inode;*(u64 *)(out+done+8)=dir.next_off;
        *(u16 *)(out+done+16)=length;out[done+18]=entry->inode_type==1?8:entry->inode_type==2?4:entry->inode_type==7?10:0;
        memcpy(out+done+19,entry->name,entry->name_length);done+=length;of->offset=dir.next_off;
    }
    ext4_dir_close(&dir);return done;
}
static i64 native_remove_directory(const char *path){
    if(ns_equal(path,"/")||ns_equal(path,"/exchange")||ns_equal(path,"/aurorafs"))return -16;
    if(aurorafs_path(path))return vfs_kind(path)==1?-20:-2;
    if(fat_ready&&fat_path(path)){FRESULT error=f_unlink(fat_name(path));if(error)return error==FR_DENIED?-39:fat_error(error);}
    else{
        if(!ext2_ready)return -38;ext4_dir dir;int error=ext4_dir_open(&dir,path);if(error)return -error;
        const ext4_direntry *entry;int nonempty=0;
        while((entry=ext4_dir_entry_next(&dir)))if(!(entry->name_length==1&&entry->name[0]=='.')&&!(entry->name_length==2&&entry->name[0]=='.'&&entry->name[1]=='.'))nonempty=1;
        ext4_dir_close(&dir);if(nonempty)return -39;error=ext4_dir_rm(path);if(error)return -error;
    }
    for(u32 i=0;i<native_count;i++)if(ns_equal(NFILES[i].path,path))NFILES[i].kind=0;return 0;
}
static int native_fd_allocate(void){for(int i=0;i<NATIVE_FDS;i++)if(!native_process[current_task].fd[i].kind)return i;return -24;}
static i64 native_open(const char *path,u32 flags,u32 mode){
    NativeProcess *p=&native_process[current_task];int fd=native_fd_allocate();if(fd<0)return fd;
    if(ns_equal(path,"/dev/urandom")||ns_equal(path,"/dev/random")){
        if(flags&3)return -13;if(!entropy_ready)return -19;int description=native_description(flags);if(!description)return -23;
        p->fd[fd]=(NativeFd){.kind=7,.description=description,.flags=flags&0x80000U};return fd;}
    if(ns_equal(path,"/dev/disk")||ns_equal(path,"/dev/boot")){int device=path[5]=='b';
        /* Raw devices are read-only: checkers inspect volumes the kernel has mounted. */
        if(device?!fs_ready:!native_ready)return -19;if(flags&3)return -13;int description=native_description(flags);if(!description)return -23;
        p->fd[fd]=(NativeFd){.kind=9,.index=device,.description=description,.flags=flags&0x80000U};return fd;}
    int index=native_find(path);
    if(index<0){if(!(flags&64))return -2;index=native_create(path);if(index<0)return index;
        *(u32 *)NFILES[index].pad=mode&0777&~p->umask;*(u32 *)(NFILES[index].pad+4)=1;if(!native_commit(index))return -5;}
    else if((flags&192)==192)return -17;
    u32 permissions=*(u32 *)(NFILES[index].pad+4)?*(u32 *)NFILES[index].pad:0755;
    if(((flags&3)!=1&&!(permissions&0400))||((flags&3)&&!(permissions&0200)))return -13;
    if((flags&0x10000)&&NFILES[index].kind!=2)return -20;
    if(NFILES[index].kind==2&&(flags&3))return -21;
    if((flags&3)&&!native_writable(path)&&!ns_equal(path,"/dev/null"))return -30;
    if(flags&512){if(NFILES[index].kind==2)return -21;NFILES[index].size=0;if(!native_commit(index))return -5;}
    int description=native_description(flags);if(!description)return -23;
    p->fd[fd]=(NativeFd){.kind=ns_equal(path,"/dev/null")?5:1,.index=index,.flags=flags&0x80000U,.description=description};return fd;
}
static i64 native_console_locked(const u8 *buffer,u64 size){
    Task *target=&tasks[DESKTOP];u64 done=0;
    while(done<size){if(target->count==QUEUE_SIZE)return done?(i64)done:-11;
        Message m={current_task,MSG_CONSOLE,0,0,0};u64 n=size-done;if(n>23)n=23;memcpy(&m.a,buffer+done,n);
        if(target->state==WAITING){void *dest=user_buffer(DESKTOP,target->receive_address,sizeof(m),1);if(!dest)return -5;
            memcpy(dest,&m,sizeof(m));target->frame.rax=0;target->state=RUNNABLE;
        }else{target->queue[(target->head+target->count)%QUEUE_SIZE]=m;target->count++;}
        for(u64 i=0;i<n;i++){while(!(inb(0x3fd)&32)){}outb(0x3f8,buffer[done+i]);}done+=n;
    }return done;
}
static i64 native_console(const u8 *buffer,u64 size){spin_lock(&ipc_locks[DESKTOP]);i64 result=native_console_locked(buffer,size);spin_unlock(&ipc_locks[DESKTOP]);return result;}
static i64 native_terminal_input(u64 id,u64 character){
    if(id>=TASK_COUNT||!native_active[id]||tasks[id].state==DEAD)return ERR_NOT_FOUND;
    NativeTty *tty=NATIVE_TTY;u8 byte=character;u32 flags=*(u32 *)(tty->termios+12);
    if((flags&1)&&(byte==3||byte==26||byte==28)){
        u32 signal=byte==3?2:byte==26?20:3;
        for(u32 i=APP_FIRST;i<TASK_COUNT;i++)if(native_active[i]&&tasks[i].state!=DEAD&&native_process[i].pgid==(int)tty->foreground)native_signal_queue(i,signal);
        tty->size=tty->ready=0;
    }else if((flags&2)&&byte==4){if(tty->size)tty->ready=tty->size;else tty->eof=1;return 0;}
    else if((flags&2)&&byte==127){if(tty->size>tty->ready)tty->size--;else return 0;}
    else {if(tty->size==sizeof(tty->input))return ERR_FULL;tty->input[tty->size++]=byte;if(byte=='\n'||!(flags&2))tty->ready=tty->size;}
    if(flags&8){u32 previous=current_task;current_task=(u32)id;
        if(byte==127){u8 backspace=8;native_console(&backspace,1);}else if(byte<32&&byte!='\n'&&byte!='\t'){u8 echo[3]={'^',(u8)(byte+64),'\n'};native_console(echo,3);}else native_console(&byte,1);
        current_task=previous;
    }return 0;
}
static i64 native_io(u64 descriptor,u64 address,u64 size,int write){
    if(descriptor>=NATIVE_FDS)return -9;NativeFd *f=&native_process[current_task].fd[descriptor];if(!f->kind)return -9;
    NativeDescription *of=&native_descriptions[f->description];
    if((write&&!(of->flags&3))||(!write&&(of->flags&3)==1))return -9;
    if(f->kind==8&&size<8)return -22;
    if(!size)return 0;void *buffer=native_buffer(current_task,address,size,!write);if(!buffer)return -14;
    if(f->kind==6)return write?network_send(f->index,buffer,size,0,0,0):network_recv(f->index,buffer,size,0,0,0);
    if(f->kind==7)return write?-9:entropy_fill(buffer,size>256?256:size);
    if(f->kind==9){if(write)return -9;i64 result=native_device_read(f->index,of->offset,buffer,size);if(result>0)of->offset+=result;return result;}
    if(f->kind==8){
        if(write){u64 value=*(u64 *)buffer;if(value==~0ULL)return -22;if(value>~1ULL-of->offset)return -11;of->offset+=value;}
        else{if(!of->offset)return -11;*(u64 *)buffer=f->index?1:of->offset;if(f->index)of->offset--;else of->offset=0;}
        return 8;
    }
    if(f->kind==4){NativeTty *tty=NATIVE_TTY;int background=(int)tty->foreground!=native_process[current_task].pgid;
        /* Background reads stop the group with SIGTTIN; writes do so with
           SIGTTOU only under TOSTOP. Ignored or blocked stop signals yield EIO
           for reads and let writes through, as on Linux. */
        if(write){if(background&&(*(u32 *)(tty->termios+12)&0x100)){if(native_actions(current_task)[22].handler==1||(native_process[current_task].sigmask&(1ULL<<21)))return native_console(buffer,size);native_signal_queue(current_task,22);return -11;}return native_console(buffer,size);}
        if(background){if(native_actions(current_task)[21].handler==1||(native_process[current_task].sigmask&(1ULL<<20)))return -5;native_signal_queue(current_task,21);return -11;}
        if(tty->eof){tty->eof=0;return 0;}if(!tty->ready)return -11;if(size>tty->ready)size=tty->ready;
        memcpy(buffer,tty->input,size);for(u64 i=size;i<tty->size;i++)tty->input[i-size]=tty->input[i];tty->size-=size;tty->ready-=size;return size;}
    if(f->kind==5)return write?(i64)size:0;
    if(f->kind==2||f->kind==3){NativePipe *p=&native_pipes[f->index];
        if(write){if(f->kind!=3)return -9;if(!p->readers){native_signal_queue(current_task,13);return -32;}
            if(! (NATIVE_PIPE_CAPACITY-p->size)||(size<=NATIVE_PIPE_CAPACITY&&size>NATIVE_PIPE_CAPACITY-p->size))return -11;
            if(size>NATIVE_PIPE_CAPACITY-p->size)size=NATIVE_PIPE_CAPACITY-p->size;memcpy(p->bytes+p->size,buffer,size);p->size+=size;return size;}
        if(f->kind!=2)return -9;if(!p->size)return p->writers?-11:0;if(size>p->size)size=p->size;memcpy(buffer,p->bytes,size);
        for(u64 i=size;i<p->size;i++)p->bytes[i-size]=p->bytes[i];p->size-=size;return size;
    }
    if(write&&!(of->flags&3))return -9;if(!write&&(of->flags&3)==1)return -9;
    if(write&&(of->flags&1024))of->offset=NFILES[f->index].size;
    i64 result=write?native_write(f->index,of->offset,buffer,size):native_read(f->index,of->offset,buffer,size);if(result>0)of->offset+=result;return result;
}
/* struct stat comes from the backend's own metadata: the ext2 inode supplies
 * inode number, link count, owners, sizes and times, so hard links share an
 * identity; FAT supplies its timestamps; AuroraFS slots are the inode. The
 * cached size is refreshed here so aliases of one inode do not go stale. */
static i64 native_stat(int index,u64 address,int special){
    if(!special&&ns_equal(NFILES[index].path,"/dev/null"))special=1;
    u8 *out=native_buffer(current_task,address,144,1);if(!out)return -14;memset(out,0,144);
    *(u64 *)(out+16)=1;*(u32 *)(out+28)=1000;*(u32 *)(out+32)=1000;*(u64 *)(out+56)=4096;
    if(special==9){*(u64 *)(out+8)=0x900+index;*(u32 *)(out+24)=0060000|0400;u64 size=native_device_size(index);*(u64 *)(out+48)=size;*(u64 *)(out+64)=size/512;return 0;}
    if(special){*(u64 *)out=1;*(u64 *)(out+8)=index+1;*(u32 *)(out+24)=(special==6?0140000:0020000)|0666;return 0;}
    NativeFile *f=&NFILES[index];int fat=fat_ready&&fat_path(f->path),legacy=aurorafs_path(f->path);
    *(u64 *)out=legacy?3:fat?2:1;*(u64 *)(out+8)=index+1;
    *(u32 *)(out+24)=(f->kind==2?0040000:f->kind==3?0120000:0100000)|(*(u32 *)(f->pad+4)?*(u32 *)f->pad:0755);
    if(legacy){int slot=aurorafs_slot(f);if(slot>=0){f->size=directory[slot].size;*(u64 *)(out+8)=0x41000000+slot;}}
    else if(fat){FILINFO info;if(f->path[9]=='/'&&f_stat(fat_name(f->path),&info)==FR_OK){f->size=info.fsize;u64 stamp=fat_epoch(info.fdate,info.ftime);*(u64 *)(out+72)=*(u64 *)(out+88)=*(u64 *)(out+104)=stamp;}
        u64 hash=1469598103934665603ULL;for(const char *s=f->path;*s;s++)hash=(hash^(u8)*s)*1099511628211ULL;*(u64 *)(out+8)=0x200000000ULL|(hash&0xffffffffULL);}
    else if(ext2_ready){uint32_t ino;struct ext4_inode inode;
        if(!ext4_raw_inode_fill(f->path,&ino,&inode)){
            *(u64 *)(out+8)=ino;*(u64 *)(out+16)=inode.links_count;*(u32 *)(out+24)=inode.mode;
            *(u32 *)(out+28)=inode.uid|((u32)inode.osd2.linux2.uid_high<<16);*(u32 *)(out+32)=inode.gid|((u32)inode.osd2.linux2.gid_high<<16);
            f->size=inode.size_lo|((u64)inode.size_hi<<32);*(u64 *)(out+64)=inode.blocks_count_lo;
            *(u64 *)(out+72)=inode.access_time;*(u64 *)(out+88)=inode.modification_time;*(u64 *)(out+104)=inode.change_inode_time;
        }
    }
    *(u64 *)(out+48)=f->size;if(!*(u64 *)(out+64))*(u64 *)(out+64)=(f->size+511)/512;
    return 0;
}
static i64 native_fork(Frame *frame,int vfork,u64 child_stack){
    native_vm_barrier(current_task);
    int id=native_slot();if(id<0)return -11;u32 parent=current_task;
    native_wait_reset(id);
    u64 *source=native_pt(parent);
    if(vfork){native_vm_owner[id]=native_space(parent);native_vm_refs[native_vm_owner[id]]++;native_vm_attached[id]=1;task_cr3[id]=task_cr3[parent];native_active[id]=1;}
    else{u64 lazy=0;for(u64 i=0;i<NATIVE_SIZE/4096;i++)if((native_alias_pt(parent)[i]&(PRESENT|NATIVE_LAZY))==NATIVE_LAZY)lazy++;
    if(lazy>native_available_pages()){native_lazy_commit_failures++;return -12;} /* the inherited untouched span alone must fit in RAM */
    native_tables(id);
    for(u64 i=0;i<NATIVE_SIZE/4096;i++){u64 alias=native_alias_pt(parent)[i];
        if(alias&1){u64 physical=alias&0x000ffffffffff000ULL;
            NATIVE_PAGE_REFS[(physical-NATIVE_PAGE_FIRST)/4096]++;
            if((source[i]&WRITE)&&!(source[i]&NATIVE_SHARED))source[i]=(source[i]&~WRITE)|NATIVE_COW;
            native_pt(id)[i]=source[i];native_alias_pt(id)[i]=alias;}
        else if(alias&NATIVE_LAZY){native_pt(id)[i]=source[i];native_alias_pt(id)[i]=NATIVE_LAZY;native_lazy_pages++;}
    }}
    native_process[id]=native_process[parent];task_affinity[id]=task_affinity[parent];NativeProcess *p=&native_process[id];p->parent=native_process[parent].tgid-100;p->vfork_parent=vfork?(int)parent:-1;p->reaped=0;
    p->fd=native_fd_tables[id];memcpy(p->fd,native_process[parent].fd,sizeof(native_fd_tables[id]));p->fd_owner=p->fs_owner=p->signal_owner=id;native_fd_users[id]=1;
    p->tgid=id+100;native_group_refs[id]=1;p->thread=0;p->clear_tid=p->robust_head=0;p->brk=native_process[native_space(parent)].brk;
    p->restore_mask_valid=0;p->continued=0;p->stopped_signal=0;for(int t=0;t<3;t++)p->timer_deadline[t]=p->timer_interval[t]=0;
    *native_signals(id)=*native_signals(parent);native_signals(id)->pending=0;
    memcpy(native_actions(id),native_actions(parent),65*sizeof(NativeSigaction));
    for(int i=0;i<NATIVE_FDS;i++){NativeFd *f=&p->fd[i];if(f->kind)native_descriptions[f->description].refs++;if(f->kind==2)native_pipes[f->index].readers++;if(f->kind==3)native_pipes[f->index].writers++;}
    memset(&tasks[id],0,sizeof(Task));tasks[id].frame=*frame;tasks[id].frame.rax=0;if(child_stack)tasks[id].frame.rsp=child_stack;
    task_fsbase[id]=task_fsbase[parent];task_gsbase[id]=task_gsbase[parent];memcpy(task_fp[id],task_fp[parent],512);exit_codes[id]=0;
    if(vfork)tasks[parent].state=3;return id+100;
}
static i64 native_exec_call(u64 path_address,u64 argv_address,u64 env_address){
    char path[256];if(!native_user_path(path_address,path))return -14;int index=native_find(path);if(index<0)return -2;
    exec_argc=exec_envc=0;
    u64 used=0;
    for(int i=0;;i++){u64 *a=native_buffer(current_task,argv_address+i*8,8,0);if(!a)return -14;if(!*a)break;
        if(i==NATIVE_EXEC_ARGS||used==NATIVE_EXEC_BYTES)return -7;
        exec_args[i]=exec_strings+used;u64 capacity=NATIVE_EXEC_BYTES-used;if(capacity>4096)capacity=4096;
        if(!native_string(*a,exec_args[i],capacity))return -7;used+=ns_length(exec_args[i])+1;exec_argc++;}
    if(env_address)for(int i=0;i<128;i++){u64 *a=native_buffer(current_task,env_address+i*8,8,0);if(!a)return -14;if(!*a)break;
        if(!native_string(*a,exec_env[exec_envc],4096))return -7;exec_envc++;if(i==127)return -7;}
    return native_exec(current_task,index);
}
static i64 native_sync(void){int error=ext2_ready?ext2_sync():0;if(error)return -error;return (virtio_present?virtio_transfer(0,0,0,4):disk_flush())?0:-5;}
#include "native_wait.h"
#include "native_network.h"
#include "native_threads.h"
static void native_mark_group(u32 id,i64 code){
    u32 group=native_process[id].tgid;
    for(u32 member=APP_FIRST;member<TASK_COUNT;member++)if(native_active[member]&&native_process[member].tgid==group&&(tasks[member].state!=DEAD||member==id)){
        tasks[member].state=DEAD;exit_codes[member]=code;native_reap[member]=1;
        if(task_cpu[member]>=0&&(u32)task_cpu[member]!=cpu_local()->index)cpu_ipi(cpus[task_cpu[member]].apic_id,62);}
}
static void native_reap_pending(void){
    if(filesystem_owner>=0)return;filesystem_enter();
    for(u32 id=APP_FIRST;id<TASK_COUNT;id++)if(native_reap[id]&&id!=current_task&&task_cpu[id]<0){native_reap[id]=0;native_finish(id,exit_codes[id]);}
    filesystem_leave();
}
/* Syscalls that may change a volume; the first one after a sync marks ext2 in use again. */
static int native_mutates(Frame *f){u64 n=f->rax;NativeProcess *p=&native_process[current_task];
    switch(n){case 1:case 18:case 20:case 296:return f->rdi<NATIVE_FDS&&p->fd[f->rdi].kind==1;
    case 2:return (f->rsi&(64|512|3))!=0;case 257:return (f->rdx&(64|512|3))!=0;
    case 76:case 77:case 82:case 83:case 84:case 86:case 87:case 88:case 90:case 91:case 92:case 93:case 94:case 132:case 235:
    case 258:case 260:case 261:case 263:case 264:case 265:case 266:case 268:case 280:case 285:case 316:case 452:return 1;
    default:return 0;}
}
static Frame *native_dispatch(Frame *f){
    u64 n=f->rax,a=f->rdi,b=f->rsi,c=f->rdx,d=f->r10,e=f->r8,g=f->r9;
    NativeProcess *p=&native_process[current_task];i64 result=-38;char path[256];
    NativeWait *waiting=&native_waits[current_task];
    if(waiting->kind&&waiting->syscall==n&&waiting->rip==f->rip&&waiting->interrupted){
        if(waiting->kind==5&&waiting->address&&!waiting->extra[0]){u64 *left=native_buffer(current_task,waiting->address,16,1);if(left){u64 ticks=waiting->deadline>timer_ticks?waiting->deadline-timer_ticks:0;left[0]=ticks/100;left[1]=(ticks%100)*10000000;}}
        native_wait_reset(current_task);tasks[current_task].frame.rax=-4;return schedule();}
    switch(n){
    case 41:case 42:case 43:case 44:case 45:case 46:case 47:case 48:case 49:case 50:case 51:case 52:case 53:case 54:case 55:
        result=native_network(n,a,b,c,d,e,g);break;
    case 7:case 271:result=native_poll_call(n,a,b,c,d,e);break;
    case 23:case 270:result=native_select_call(n,a,b,c,d,e,g);break;
    case 35:case 230:result=native_sleep_call(n,a,b,c,d);break;
    case 284:case 290:{
        u64 flags=n==290?b:0;if(flags&~(0x80000ULL|0x800ULL|1ULL)){result=-22;break;}
        int fd=native_fd_allocate();if(fd<0){result=fd;break;}int description=native_description(2|(flags&0x800));
        if(!description){result=-23;break;}native_descriptions[description].offset=(u32)a;
        p->fd[fd]=(NativeFd){.kind=8,.index=flags&1,.description=description,.flags=flags&0x80000};result=fd;break;
    }
    case 0:case 1:result=native_io(a,b,c,n==1);break;
    case 2:case 257:{u64 address=n==2?a:b;u32 flags=n==2?b:c;
        result=native_at_path(n==257?(i64)a:-100,address,path);if(!result)result=native_open(path,flags,n==257?d:c);break;}
    case 3:if(a>=NATIVE_FDS||!p->fd[a].kind)result=-9;else{native_close(current_task,a);result=0;}break;
    case 4:case 6:case 262:{u64 address=n==262?b:a,target=n==262?c:b;
        result=native_at_path(n==262?(i64)a:-100,address,path);if(result)break;int index;
        if(ext2_ready&&!native_foreign(path)&&(n==6||(n==262&&(d&256)))){char resolved[256];result=ext2_resolve(resolved,path,0);if(result)break;
            index=-1;for(u32 i=0;i<native_count;i++)if(NFILES[i].kind&&ns_equal(NFILES[i].path,resolved)){index=i;break;}if(index<0)index=ext2_find(resolved);
        }else index=native_find(path);result=index<0?-2:native_stat(index,target,0);break;}
    case 5:result=a>=NATIVE_FDS||!p->fd[a].kind?-9:native_stat(p->fd[a].index,b,p->fd[a].kind==6?6:p->fd[a].kind==9?9:p->fd[a].kind!=1);break;
    case 8:if(a>=NATIVE_FDS||!p->fd[a].kind){result=-9;break;}if(p->fd[a].kind==5){result=c>4?-22:0;break;}if(p->fd[a].kind!=1&&p->fd[a].kind!=9){result=-29;break;}
        {i64 position=(i64)b;if(c==1)position+=(i64)native_descriptions[p->fd[a].description].offset;else if(c==2)position+=(i64)(p->fd[a].kind==9?native_device_size(p->fd[a].index):NFILES[p->fd[a].index].size);else if(c!=0){result=-22;break;}
        if(position<0)result=-22;else result=native_descriptions[p->fd[a].description].offset=(u64)position;}break;
    case 9:{u64 size=(b+4095)&~4095ULL;if(!b||b>NATIVE_SIZE||!size||(c&6)==6){result=-22;break;}
        if((d&3)!=1&&(d&3)!=2){result=-22;break;}if((d&1)&&!(d&32)){result=-95;break;}
        u64 address=a;if(!(d&16))address=native_mapping_gap(current_task,size);
        if(address&4095){result=-22;break;}
        if(address<USER_BASE||address>=NATIVE_STACK-4096||size>NATIVE_STACK-4096-address){result=-12;break;}
        if(!(d&32)&&(e>=NATIVE_FDS||p->fd[e].kind!=1)){result=-9;break;}
        /* Anonymous private pages are committed lazily; MAP_FIXED discards
           what was there so the new range reads as zeros. File-backed and
           shared mappings are populated now. */
        int lazy=(d&32)&&!(d&1);u64 first=(address-USER_BASE)/4096,last=(address+size-USER_BASE)/4096;
        if((d&16)&&lazy)for(u64 i=first;i<last;i++)native_unmap_page(current_task,i);
        if(!native_map_pages(current_task,address,size,((c&2)?WRITE:0)|((c&4)?0:NX)|((d&1)?NATIVE_SHARED:0),lazy)){result=-12;break;}
        if((d&16)&&!lazy)memset((void *)(native_phys(current_task)+address-USER_BASE),0,size);
        if(!c)for(u64 i=first;i<last;i++){u64 *pt=native_pt(current_task);if(pt[i]&NATIVE_LAZY)pt[i]|=NATIVE_NONE;else pt[i]&=~1ULL;}
        if(!(d&32)){result=native_read(p->fd[e].index,g,(void *)(native_phys(current_task)+address-USER_BASE),b);if(result<0)break;}
        result=address;break;}
    case 10:native_vm_barrier(current_task);if((a&4095)||a<USER_BASE||!b||a>=NATIVE_END||b>NATIVE_END-a||(c&6)==6){result=-22;break;}
        result=0;for(u64 i=(a-USER_BASE)/4096;i<=(a+b-1-USER_BASE)/4096;i++)if(!native_mapped(native_alias_pt(current_task)[i]))result=-12;
        if(result)break;for(u64 i=(a-USER_BASE)/4096;i<=(a+b-1-USER_BASE)/4096;i++){u64 *pt=native_pt(current_task);
            if(pt[i]&NATIVE_LAZY){pt[i]=NATIVE_LAZY|USER|((c&2)?WRITE:0)|((c&4)?0:NX)|(c?0:NATIVE_NONE);continue;}
            u64 physical=pt[i]&0x000ffffffffff000ULL;u64 writable=(c&2)?(NATIVE_PAGE_REFS[(physical-NATIVE_PAGE_FIRST)/4096]>1&&!(pt[i]&NATIVE_SHARED)?NATIVE_COW:WRITE):0;pt[i]=physical|(pt[i]&NATIVE_SHARED)|(c?PRESENT:0)|USER|writable|((c&4)?0:NX);}break;
    case 11:if((a&4095)||a<USER_BASE||!b||a>=NATIVE_END||b>NATIVE_END-a){result=-22;break;}
        for(u64 i=(a-USER_BASE)/4096;i<=(a+b-1-USER_BASE)/4096;i++)native_unmap_page(current_task,i);result=0;break;
    case 12:{NativeProcess *vm=&native_process[native_space(current_task)];
        if(a>=vm->min_brk&&a<NATIVE_MMAP_BASE-4096){
            if(a>vm->brk){if(!native_map_pages(current_task,vm->brk,a-vm->brk,WRITE|NX,1)){result=vm->brk;break;}}
            else for(u64 address=(a+4095)&~4095ULL;address<vm->brk;address+=4096)native_unmap_page(current_task,(address-USER_BASE)/4096);
            vm->brk=a;}result=vm->brk;break;}
    case 13:{if(!a||a>64||d!=8||(b&&(a==9||a==19))){result=-22;break;}NativeSigaction *in=b?native_buffer(current_task,b,32,0):0,*out=c?native_buffer(current_task,c,32,1):0;
        if((b&&!in)||(c&&!out)){result=-14;break;}NativeSigaction next;if(in)next=*in;if(out)*out=native_actions(current_task)[a];if(in)native_actions(current_task)[a]=next;result=0;break;}
    case 15:if(!native_sigreturn(current_task))native_mark_group(current_task,-11);return schedule();
    case 25:result=native_mremap(current_task,a,b,c,d,e);break;
    case 26:if((a&4095)||(c&~7ULL)||((c&1)&&(c&4))){result=-22;break;}result=b&&!native_buffer(current_task,a,b,0)?-12:native_sync();break;
    case 149:case 150:case 151:case 152:case 325:if(n==149||n==150||n==325){if(a&4095){result=-22;break;}result=!b||native_buffer(current_task,a,b,0)?0:-12;}else result=0;break;
    case 34:if(!waiting->kind)*waiting=(NativeWait){.kind=7,.syscall=n,.deadline=~0ULL};result=-4096;break;
    case 130:{if(b!=8){result=-22;break;}
        if(!waiting->kind){u64 *in=native_buffer(current_task,a,8,0);if(!in){result=-14;break;}
            *waiting=(NativeWait){.kind=7,.syscall=n,.deadline=~0ULL,.oldmask=p->sigmask,.mask_changed=1};p->sigmask=*in&~((1ULL<<8)|(1ULL<<18));}
        result=-4096;break;}
    case 127:{if(b!=8){result=-22;break;}u64 *out=native_buffer(current_task,a,8,1);if(!out){result=-14;break;}*out=native_signals(current_task)->pending;result=0;break;}
    case 128:{if(d!=8){result=-22;break;}NativeSignals *s=native_signals(current_task);
        if(!waiting->kind){u64 *set=native_buffer(current_task,a,8,0);if(!set){result=-14;break;}
            *waiting=(NativeWait){.kind=8,.syscall=n,.bits=*set&~((1ULL<<8)|(1ULL<<18))};int error=native_deadline(c,0,0,0,&waiting->deadline);if(error){native_wait_reset(current_task);result=error;break;}}
        u64 ready=s->pending&waiting->bits;
        if(ready){u32 signal=1;while(!(ready&1)){signal++;ready>>=1;}s->pending&=~(1ULL<<(signal-1));
            if(b){u8 *info=native_buffer(current_task,b,128,1);if(!info){result=-14;break;}memset(info,0,128);*(u32 *)info=signal;*(int *)(info+8)=s->code[signal];
                *(u32 *)(info+16)=s->sender[signal];*(u32 *)(info+20)=1000;*(u64 *)(info+24)=s->value[signal];}
            result=signal;break;}
        result=timer_ticks>=waiting->deadline?-11:-4096;break;}
    case 129:case 297:{u64 group=a,signal=n==129?b:c,info=n==129?c:d;if(signal>64){result=-22;break;}
        u8 *in=native_buffer(current_task,info,128,0);if(!in){result=-14;break;}int code=*(int *)(in+8);if(code>=0){result=-1;break;}
        u64 target=n==129?group:b;result=-3;
        for(u32 id=APP_FIRST;id<TASK_COUNT;id++)if(native_active[id]&&tasks[id].state!=DEAD&&(n==129?native_process[id].tgid==group&&!native_process[id].thread:id+100==target&&native_process[id].tgid==group)){
            if(signal)native_signal_post(id,signal,p->tgid,code,*(u64 *)(in+24));result=0;break;}
        break;}
    case 37:case 36:case 38:{NativeProcess *leader=&native_process[p->tgid-100];int which=n==37?0:(int)a;if(which<0||which>2){result=-22;break;}
        u64 remaining=leader->timer_deadline[which]>timer_ticks?leader->timer_deadline[which]-timer_ticks:0,interval=leader->timer_interval[which];
        if(n==37){leader->timer_deadline[0]=a?timer_ticks+a*100:0;leader->timer_interval[0]=0;result=(remaining+99)/100;break;}
        if(n==38&&b){u64 *in=native_buffer(current_task,b,32,0);if(!in){result=-14;break;}
            if((i64)in[0]<0||(i64)in[1]<0||(i64)in[2]<0||(i64)in[3]<0||in[1]>=1000000||in[3]>=1000000){result=-22;break;}
            u64 next=in[2]*100+(in[3]+9999)/10000,period=in[0]*100+(in[1]+9999)/10000;
            if(in[2]|in[3]){if(!next)next=1;}if((in[0]|in[1])&&!period)period=1;
            leader->timer_deadline[which]=next?timer_ticks+next:0;leader->timer_interval[which]=period;}
        u64 target=n==36?b:c;result=0;if(target){u64 *out=native_buffer(current_task,target,32,1);if(!out){result=-14;break;}
            out[0]=interval/100;out[1]=(interval%100)*10000;out[2]=remaining/100;out[3]=(remaining%100)*10000;}break;}
    case 100:{u64 *out=native_buffer(current_task,a,32,1);if(!out){result=-14;break;}u64 ticks=task_runs[current_task];out[0]=ticks;out[1]=ticks/8;out[2]=out[3]=0;result=timer_ticks;break;}
    case 140:result=20;break; /* nice 0, encoded as 20-nice */
    case 141:result=(int)c<-20||(int)c>19?-22:0;break;
    case 142:{u64 *param=native_buffer(current_task,b,4,0);result=!param?-14:(int)*(u32 *)param?-22:0;break;}
    case 143:{u32 *out=native_buffer(current_task,b,4,1);if(!out)result=-14;else{*out=0;result=0;}break;}
    case 144:{u32 *param=native_buffer(current_task,c,4,0);result=!param?-14:b?-22:*param?-22:0;break;}
    case 145:result=0;break;
    case 146:result=a==1||a==2?99:a==0||a==3||a==5?0:-22;break;
    case 147:result=a==1||a==2?1:a==0||a==3||a==5?0:-22;break;
    case 148:{u64 *out=native_buffer(current_task,b,16,1);if(!out){result=-14;break;}out[0]=0;out[1]=10000000;result=0;}break;
    case 157:if(a==15){if(!native_string(b,path,16)){char *in=native_buffer(current_task,b,15,0);if(!in){result=-14;break;}memcpy(path,in,15);path[15]=0;}
            memcpy(p->name,path,16);result=0;}
        else if(a==16){char *out=native_buffer(current_task,b,16,1);if(!out){result=-14;break;}memcpy(out,p->name,16);result=0;}
        else if(a==1||a==38||a==22){result=0;}else if(a==2||a==39){u32 *out=native_buffer(current_task,b,4,1);if(!out){result=-14;break;}*out=0;result=0;}
        else if(a==0x53564d41)result=0;else result=-22;break;
    case 160:{if(a>=16){result=-22;break;}u64 *in=native_buffer(current_task,b,16,0);result=in?0:-14;break;} /* limits are advisory */
    case 324:result=a==0?0x7f:(a&~0x7fULL)?-22:0;break;
    case 14:if(d!=8){result=-22;break;}if(c){u64 *out=native_buffer(current_task,c,8,1);if(!out){result=-14;break;}*out=p->sigmask;}
        if(b){u64 *in=native_buffer(current_task,b,8,0);if(!in){result=-14;break;}if(a==0)p->sigmask|=*in;else if(a==1)p->sigmask&=~*in;else if(a==2)p->sigmask=*in;else{result=-22;break;}}p->sigmask&=~((1ULL<<8)|(1ULL<<18));result=0;break;
    case 16:{if(a>=NATIVE_FDS||!p->fd[a].kind){result=-9;break;}
        if(p->fd[a].kind==6){if(b!=0x541b&&b!=0x5421){result=-25;break;}u32 *value=native_buffer(current_task,c,4,b==0x541b);if(!value){result=-14;break;}
            if(b==0x541b)*value=network_available(p->fd[a].index);else{NativeDescription *of=&native_descriptions[p->fd[a].description];of->flags=(of->flags&~0x800U)|(*value?0x800:0);}result=0;break;}
        if(p->fd[a].kind!=4){result=-25;break;}NativeTty *tty=NATIVE_TTY;
        u64 length=b==0x5401||b==0x5402||b==0x5403||b==0x5404?36:b==0x5413?8:4;
        if(b==0x540e){result=0;break;}if(b==0x540b){tty->size=tty->ready=tty->eof=0;result=0;break;}
        void *buffer=native_buffer(current_task,c,length,b==0x5401||b==0x540f||b==0x5413||b==0x541b||b==0x5424);
        if(!buffer){result=-14;break;}result=0;
        if(b==0x5401)memcpy(buffer,tty->termios,36);
        else if(b==0x5402||b==0x5403||b==0x5404){memcpy(tty->termios,buffer,36);if(b==0x5404)tty->size=tty->ready=0;}
        else if(b==0x540f)*(u32 *)buffer=tty->foreground;
        else if(b==0x5410)tty->foreground=*(u32 *)buffer;
        else if(b==0x5413){u16 *size=buffer;size[0]=17;size[1]=73;size[2]=size[3]=0;}
        else if(b==0x541b)*(u32 *)buffer=tty->ready;else if(b==0x5424)*(u32 *)buffer=0;else result=-25;break;}
    case 17:if(a>=NATIVE_FDS||(p->fd[a].kind!=1&&p->fd[a].kind!=9)){result=-9;break;}{void *out=c?native_buffer(current_task,b,c,1):0;result=c&&!out?-14:p->fd[a].kind==9?native_device_read(p->fd[a].index,d,out,c):native_read(p->fd[a].index,d,out,c);}break;
    case 18:{if(a>=NATIVE_FDS||!p->fd[a].kind){result=-9;break;}NativeFd *fd=&p->fd[a];
        if(fd->kind!=1){result=-29;break;}if(!(native_descriptions[fd->description].flags&3)){result=-9;break;}
        if((i64)d<0){result=-22;break;}void *in=c?native_buffer(current_task,b,c,0):0;
        result=c&&!in?-14:native_write(fd->index,d,in,c);break;}
    case 19:case 20:{if(c>1024){result=-22;break;}u64 *iov=native_buffer(current_task,b,c*16,0);if(!iov){result=-14;break;}result=0;
        for(u64 i=0;i<c;i++){i64 amount=native_io(a,iov[i*2],iov[i*2+1],n==20);if(amount<0){if(!result)result=amount;break;}result+=amount;if((u64)amount<iov[i*2+1])break;}break;}
    case 21:case 269:case 439:{result=native_at_path(n==21?-100:(i64)a,n==21?a:b,path);if(result)break;int index=native_find(path);if(index<0){result=-2;break;}
        u32 mode=n==21?b:c,permissions=*(u32 *)(NFILES[index].pad+4)?*(u32 *)NFILES[index].pad:0755;
        result=mode&~7U?-22:((mode&4)&&!(permissions&0400))||((mode&2)&&!(permissions&0200))||((mode&1)&&!(permissions&0100))?-13:0;break;}
    case 22:case 293:{int *out=native_buffer(current_task,a,8,1);if(!out){result=-14;break;}int pipe;
        if(n==293&&(b&~0x80800ULL)){result=-22;break;}
        for(pipe=0;pipe<16;pipe++)if(!native_pipes[pipe].readers&&!native_pipes[pipe].writers)break;
        if(pipe==16){result=-24;break;}int r=native_fd_allocate();if(r<0){result=r;break;}p->fd[r]=(NativeFd){.kind=2,.index=pipe,.flags=n==293?(u32)b:0};
        int w=native_fd_allocate();if(w<0){memset(&p->fd[r],0,sizeof(NativeFd));result=w;break;}
        p->fd[w]=(NativeFd){.kind=3,.index=pipe,.flags=n==293?(u32)b:0};
        p->fd[r].description=native_description(n==293?(u32)b:0);p->fd[w].description=native_description((n==293?(u32)b:0)|1);
        if(!p->fd[r].description||!p->fd[w].description){native_close(current_task,r);native_close(current_task,w);result=-23;break;}
        native_pipes[pipe]=(NativePipe){.readers=1,.writers=1};out[0]=r;out[1]=w;result=0;break;}
    case 24:result=0;break;
    case 27:{if((a&4095)||a<USER_BASE||a>=NATIVE_END||!b||b>NATIVE_END-a){result=-22;break;}
        u64 count=(b+4095)/4096;u8 *out=native_buffer(current_task,c,count,1);if(!out){result=-14;break;}
        /* Lazily committed pages are mapped but not yet resident. */
        result=0;for(u64 i=0;i<count;i++){u64 alias=native_alias_pt(current_task)[(a-USER_BASE)/4096+i];if(!native_mapped(alias)){result=-12;break;}out[i]=(alias&1)?1:0;}
        break;}
    case 28:result=0;break; /* madvise: advisory only */
    case 32:case 33:case 292:{if(a>=NATIVE_FDS||!p->fd[a].kind){result=-9;break;}int fd=n==32?native_fd_allocate():(int)b;
        if(n==292&&(a==b||(c&~0x80000ULL))){result=-22;break;}
        if(fd<0||fd>=NATIVE_FDS){result=-9;break;}if((u64)fd!=a){native_close(current_task,fd);p->fd[fd]=p->fd[a];p->fd[fd].flags&=~0x80000U;
            native_descriptions[p->fd[fd].description].refs++;
            if(n==292)p->fd[fd].flags|=(u32)c;if(p->fd[fd].kind==2)native_pipes[p->fd[fd].index].readers++;if(p->fd[fd].kind==3)native_pipes[p->fd[fd].index].writers++;}result=fd;break;}
    case 39:result=p->tgid;break;
    case 115:if((int)a<0)result=-22;else if(!a)result=1;else {u32 *out=native_buffer(current_task,b,4,1);if(!out)result=-14;else {*out=1000;result=1;}}break;
    case 131:{NativeSignals *s=native_signals(current_task);u64 *in=a?native_buffer(current_task,a,24,0):0,*out=b?native_buffer(current_task,b,24,1):0;
        if((a&&!in)||(b&&!out)){result=-14;break;}u64 next[3];if(in)memcpy(next,in,24);
        int on_alt=native_on_alt(s,f->rsp);
        if(out){out[0]=s->alt_sp;out[1]=on_alt?1:s->alt_size?0:2;out[2]=s->alt_size;}
        result=0;if(in){u32 flags=next[1];if(on_alt)result=-1;else if(flags&~0x80000002U)result=-22;
            else if(flags&2){s->alt_sp=s->alt_size=0;}else if(next[2]<2048)result=-12;
            else if(!native_buffer(current_task,next[0],next[2],1))result=-14;
            else{s->alt_sp=next[0];s->alt_size=next[2];}}break;}
    case 229:if(a!=0&&a!=1&&a!=4&&a!=5&&a!=6&&a!=7)result=-22;else if(!b)result=0;else{u64 *out=native_buffer(current_task,b,16,1);if(!out)result=-14;else{out[0]=0;out[1]=10000000;result=0;}}break;
    case 186:result=current_task+100;break;
    case 56:result=(a&0x10000)?native_clone_thread(f,a,b,c,d,e):((a&0x100)&&!(a&0x4000)?-38:native_fork(f,(a&0x4000)!=0,b));break;
    case 57:case 58:result=native_fork(f,n==58,0);break;
    case 59:result=native_exec_call(a,b,c);if(!result)return schedule();break;
    case 60:native_finish(current_task,(int)a);return schedule();
    case 231:native_mark_group(current_task,(int)a);return schedule();
    case 62:case 200:case 234:{u64 target=n==234?b:a,signal=n==234?c:b;
        if(signal>64){result=-22;break;}result=-3;
        if(n==234&&(b<100+APP_FIRST||b>=100+TASK_COUNT||native_process[b-100].tgid!=a))break;
        for(u32 id=APP_FIRST;id<TASK_COUNT;id++)if(native_active[id]&&tasks[id].state!=DEAD&&
            (target==id+100||(n==62&&((i64)target==-1||(!target&&native_process[id].pgid==p->pgid)||((i64)target<-1&&native_process[id].pgid==-(i64)target))))){
            if(signal)native_signal_post(id,signal,p->tgid,n==62?0:-6,0);result=0;
        }
        break;}
    case 61:case 247:{
        /* wait4(pid,status,options,rusage) and waitid(idtype,id,infop,options,rusage).
           Children report exits, stops (WUNTRACED) and SIGCONT (WCONTINUED). */
        u64 options=n==61?c:d;if(options&~0x4000000fULL){result=-22;break;}
        i64 which=n==61?(i64)a:a==0?-1:a==1?(i64)b:a==2?(b?-(i64)b:0):-2;
        if(which==-2||(n==247&&!(options&0xe))){result=-22;break;}
        int found=0,child=-1,kind=0;for(int i=APP_FIRST;i<APP_FIRST+NATIVE_SLOTS;i++)if(native_active[i]&&native_process[i].parent==(int)p->tgid-100&&!native_process[i].thread&&!native_process[i].reaped&&
            (which==-1||which==i+100||(which==0&&native_process[i].pgid==p->pgid)||(which<-1&&native_process[i].pgid==-which))){
            found=1;
            if(tasks[i].state==DEAD&&!native_group_refs[i]&&(n==61||(options&4))){child=i;kind=1;break;}
            if(tasks[i].state==STOPPED&&(options&2)&&native_process[i].stopped_signal){child=i;kind=2;break;}
            if(native_process[i].continued&&(options&8)){child=i;kind=3;break;}}
        if(child<0){result=!found?-10:(options&1)?0:-11;break;}
        NativeProcess *cp=&native_process[child];int status=kind==1?(exit_codes[child]<0?(exit_codes[child]<=-128?11:(-(int)exit_codes[child]&127)):((int)exit_codes[child]&255)<<8):kind==2?(cp->stopped_signal<<8)|127:0xffff;
        int code=kind==1?(exit_codes[child]<0?2:1):kind==2?5:6,value=kind==1?(exit_codes[child]<0?(exit_codes[child]<=-128?11:(int)-exit_codes[child]):(int)exit_codes[child]&255):kind==2?cp->stopped_signal:18;
        if(n==61&&b){int *out=native_buffer(current_task,b,4,1);if(!out){result=-14;break;}*out=status;}
        if(n==247&&c){u8 *info=native_buffer(current_task,c,128,1);if(!info){result=-14;break;}memset(info,0,128);*(u32 *)info=17;*(int *)(info+8)=code;*(u32 *)(info+16)=child+100;*(u32 *)(info+20)=1000;*(int *)(info+24)=value;}
        u64 usage=n==61?d:e;if(usage){void *out=native_buffer(current_task,usage,144,1);if(!out){result=-14;break;}memset(out,0,144);}
        if(kind==1){if(!(options&0x1000000))cp->reaped=1;}else if(kind==2){if(!(options&0x1000000))cp->stopped_signal=0;}else if(!(options&0x1000000))cp->continued=0;
        result=n==61?child+100:0;break;}
    case 63:{char *out=native_buffer(current_task,a,390,1);if(!out){result=-14;break;}memset(out,0,390);ns_copy(out,"Aurora");ns_copy(out+65,"aurora");ns_copy(out+130,"0.3");ns_copy(out+195,"GCC compatibility");ns_copy(out+260,"x86_64");result=0;break;}
    case 72:if(a>=NATIVE_FDS||!p->fd[a].kind){result=-9;break;}if(b==1)result=(p->fd[a].flags&0x80000)?1:0;
        else if(b==2){p->fd[a].flags=(p->fd[a].flags&~0x80000U)|((c&1)?0x80000:0);result=0;}
        else if(b==3)result=native_descriptions[p->fd[a].description].flags;
        else if(b==4){NativeDescription *of=&native_descriptions[p->fd[a].description];of->flags=(of->flags&~0xc00U)|(c&0xc00);result=0;}
        else if(b==1031||b==1032){if(p->fd[a].kind!=2&&p->fd[a].kind!=3)result=-22;else result=b==1031&&c>NATIVE_PIPE_CAPACITY?-1:NATIVE_PIPE_CAPACITY;}
        else if(b==0||b==1030){if(c>=NATIVE_FDS){result=-22;break;}int fd;
            for(fd=(int)c;fd<NATIVE_FDS&&p->fd[fd].kind;fd++){}if(fd==NATIVE_FDS){result=-24;break;}
            p->fd[fd]=p->fd[a];p->fd[fd].flags=b==1030?0x80000:0;native_descriptions[p->fd[fd].description].refs++;
            if(p->fd[fd].kind==2)native_pipes[p->fd[fd].index].readers++;if(p->fd[fd].kind==3)native_pipes[p->fd[fd].index].writers++;result=fd;
        }else result=-22;break;
    case 74:case 75:result=a>=NATIVE_FDS||!p->fd[a].kind?-9:native_sync();break;
    case 162:result=native_sync();break;
    case 77:if(a>=NATIVE_FDS||p->fd[a].kind!=1)result=-9;else if(!(native_descriptions[p->fd[a].description].flags&3))result=-22;else if(!ext2_ready&&b>NFILES[p->fd[a].index].size)result=-22;else{NFILES[p->fd[a].index].size=b;result=native_commit(p->fd[a].index)?0:-5;}break;
    case 79:{u64 length=ns_length(p->cwd)+1;char *out=native_buffer(current_task,a,b,1);if(!out)result=-14;else if(b<length)result=-34;else{memcpy(out,p->cwd,length);result=length;}break;}
    case 80:if(!native_user_path(a,path))result=-14;else{int index=native_find(path);if(index<0)result=-2;else if(NFILES[index].kind!=2)result=-20;else{for(int i=APP_FIRST;i<TASK_COUNT;i++)if(native_active[i]&&native_process[i].fs_owner==p->fs_owner)ns_copy(native_process[i].cwd,path);result=0;}}break;
    case 81:if(a>=NATIVE_FDS||p->fd[a].kind!=1)result=-9;else if(NFILES[p->fd[a].index].kind!=2)result=-20;else{for(int i=APP_FIRST;i<TASK_COUNT;i++)if(native_active[i]&&native_process[i].fs_owner==p->fs_owner)ns_copy(native_process[i].cwd,NFILES[p->fd[a].index].path);result=0;}break;
    case 83:case 258:result=native_at_path(n==258?(i64)a:-100,n==258?b:a,path);if(result)break;
        if(aurorafs_path(path)){result=vfs_kind(path)>=0?-17:-1;break;} /* AuroraFS is flat */
        if(fat_ready&&fat_path(path)){FRESULT error=f_mkdir(fat_name(path));result=error?fat_error(error):0;break;}
        result=ext2_ready?-ext4_dir_mk(path):-38;if(!result)result=-ext4_mode_set(path,(n==258?c:b)&0777&~p->umask);break;
    case 84:result=native_user_path(a,path)?native_remove_directory(path):-14;break;
    case 86:case 265:{char target[256];if(n==265&&(e&~0x1400ULL)){result=-22;break;}
        result=native_at_path(n==86?-100:(i64)a,n==86?a:b,path);if(result)break;
        result=native_at_path(n==86?-100:(i64)c,n==86?b:d,target);if(result)break;
        if(native_foreign(path)!=native_foreign(target)){result=-18;break;}if(!ext2_ready||native_foreign(path)){result=-95;break;}
        char from[256],to[256];result=ext2_resolve(from,path,n==265&&(e&0x400));if(result)break;result=ext2_resolve(to,target,0);if(result)break;
        int kind=vfs_kind(from);if(kind<0){result=kind;break;}if(kind==2){result=-1;break;}
        int exists=vfs_kind(to);if(exists>=0){result=-17;break;}if(exists!=-2){result=exists;break;}
        result=-ext4_flink(from,to);break;}
    case 76:{if(!native_user_path(a,path)){result=-14;break;}int index=native_find(path);if(index<0){result=-2;break;}
        if(NFILES[index].kind==2){result=-21;break;}if(!native_writable(path)){result=-30;break;}
        if((i64)b<0||(!ext2_ready&&b>NFILES[index].size)){result=-22;break;}NFILES[index].size=b;result=native_commit(index)?0:-5;break;}
    case 285:{if(a>=NATIVE_FDS||p->fd[a].kind!=1){result=-9;break;}if(!(native_descriptions[p->fd[a].description].flags&3)){result=-9;break;}
        if((i64)c<0||(i64)d<=0){result=-22;break;}if(b&~1ULL){result=-95;break;}
        u64 end=c+d;if(!(b&1)&&end>NFILES[p->fd[a].index].size){if(!ext2_ready){result=-22;break;}NFILES[p->fd[a].index].size=end;result=native_commit(p->fd[a].index)?0:-5;}else result=0;break;}
    case 137:case 138:{if(n==138&&(a>=NATIVE_FDS||!p->fd[a].kind)){result=-9;break;}
        if(n==137){if(!native_user_path(a,path)){result=-14;break;}if(native_find(path)<0){result=-2;break;}}else ns_copy(path,p->fd[a].kind==1?NFILES[p->fd[a].index].path:"/");
        u64 *out=native_buffer(current_task,b,120,1);if(!out){result=-14;break;}memset(out,0,120);
        int fat=fat_ready&&fat_path(path);out[0]=fat?0x4d44:ext2_ready?0xef53:0x9fa0;out[1]=out[9]=4096;out[8]=255;
        if(fat){DWORD clusters=0;FATFS *fs;if(f_getfree(fat_name(path),&clusters,&fs)==FR_OK){out[1]=out[9]=(u64)fs->csize*512;out[2]=fs->n_fatent-2;out[3]=out[4]=clusters;}}
        else if(ext2_ready){struct ext4_mount_stats stats;if(!ext4_mount_point_stats("/",&stats)){out[1]=out[9]=stats.block_size;out[2]=stats.blocks_count;out[3]=out[4]=stats.free_blocks_count;out[5]=stats.inodes_count;out[6]=stats.free_inodes_count;}}
        result=0;break;}
    case 73:if(a>=NATIVE_FDS||!p->fd[a].kind)result=-9;else{u64 op=b&~4ULL;result=op==1||op==2||op==8?0:-22;}break; /* single-owner volume: locks are advisory */
    case 82:case 264:case 316:{char target[256];result=native_at_path(n==82?-100:(i64)a,n==82?a:b,path);if(result)break;
        result=native_at_path(n==82?-100:(i64)c,n==82?b:d,target);if(result)break;
        result=ext2_ready||native_foreign(path)?vfs_rename(path,target,n==316?(u32)e:0):-38;break;}
    case 88:case 266:if(!ext2_ready)result=-38;else{char target[256];
        if(!native_string(a,target,sizeof(target))){result=-14;break;}
        result=native_at_path(n==266?(i64)b:-100,n==266?c:b,path);if(result)break;
        if(native_foreign(path)){result=-95;break;}
        char resolved[256];result=ext2_resolve(resolved,path,0);if(result)break;
        int kind=vfs_kind(resolved);result=kind>=0?-17:kind!=-2?kind:-ext4_fsymlink(target,resolved);}break;
    case 87:case 263:{result=native_at_path(n==263?(i64)a:-100,n==263?b:a,path);if(result)break;
        if(n==263&&c){result=c==512?native_remove_directory(path):-22;break;}
        if(native_foreign(path))result=vfs_unlink(path); /* FAT and AuroraFS names park open files the same way */
        else if(ext2_ready){char resolved[256];result=ext2_resolve(resolved,path,0);if(result)break;uint32_t mode;result=-ext4_mode_get(resolved,&mode);if(result)break;
            if((mode&0170000)==0040000){result=-21;break;}result=vfs_unlink(resolved);
        }else{int index=native_find(path);if(index<0)result=-2;else if(NFILES[index].kind==2)result=-21;else if(!native_writable(path))result=-30;else{NFILES[index].kind=0;result=native_commit(index)?0:-5;}}break;}
    case 89:if(!native_user_path(a,path))result=-14;else if(ns_equal(path,"/proc/self/exe")){u64 length=ns_length(p->exe);if(length>c)length=c;void *out=native_buffer(current_task,b,length,1);if(!out)result=-14;else{memcpy(out,p->exe,length);result=length;}}
        else if(ext2_ready){void *out=native_buffer(current_task,b,c,1);size_t count=0;if(!out)result=-14;else{int error=ext4_readlink(path,out,c,&count);result=error?-error:(i64)count;}}else result=-22;break;
    case 90:case 91:case 268:case 452:{int index;u32 mode=n==268||n==452?c:b;
        if(n==268||n==452){if(n==452&&d){result=-95;break;}result=native_at_path((i64)a,b,path);if(result)break;index=native_find(path);}
        else if(n==90){if(!native_user_path(a,path)){result=-14;break;}index=native_find(path);}
        else index=a<NATIVE_FDS&&p->fd[a].kind==1?p->fd[a].index:-1;
        if(index<0){result=n==90?-2:-9;break;}if(!native_writable(NFILES[index].path)){result=-30;break;}
        *(u32 *)NFILES[index].pad=mode&07777;*(u32 *)(NFILES[index].pad+4)=1;result=native_commit(index)?0:-5;
        if(!result&&ext2_ready&&!native_foreign(NFILES[index].path))ext4_ctime_set(NFILES[index].path,native_timestamp());break;}
    case 95:result=p->umask;for(int i=APP_FIRST;i<TASK_COUNT;i++)if(native_active[i]&&native_process[i].fs_owner==p->fs_owner)native_process[i].umask=(u32)a&0777;break;
    case 92:case 93:case 94:case 260:{
        /* The single user is root-like: any owner may be recorded. lchown and
           AT_SYMLINK_NOFOLLOW stop at the link itself. -1 keeps a field. */
        u64 uid=n==260?c:b,gid=n==260?d:c;int nofollow=n==94||(n==260&&(e&256));if(n==260&&(e&~0x100ULL)){result=-22;break;}
        if(n==93){if(a>=NATIVE_FDS||p->fd[a].kind!=1){result=-9;break;}ns_copy(path,NFILES[p->fd[a].index].path);}
        else{result=n==260?native_at_path((i64)a,b,path):native_user_path(a,path)?0:-14;if(result)break;}
        if(native_foreign(path)){result=native_find(path)<0?-2:0;break;}if(!ext2_ready){result=-38;break;}
        char resolved[256];result=ext2_resolve(resolved,path,!nofollow);if(result)break;
        uint32_t old_uid,old_gid;result=-ext4_owner_get(resolved,&old_uid,&old_gid);if(result)break;
        if((u32)uid==0xffffffffU)uid=old_uid;if((u32)gid==0xffffffffU)gid=old_gid;
        result=-ext4_owner_set(resolved,(u32)uid,(u32)gid);if(!result)result=-ext4_ctime_set(resolved,native_timestamp());break;}
    case 132:case 235:case 261:{ /* utime / utimes / futimesat as utimensat */
        u64 address=n==261?b:a,times_address=n==261?c:b;i64 dirfd=n==261?(i64)a:-100;
        result=native_at_path(dirfd,address,path);if(result)break;int index=native_find(path);if(index<0){result=-2;break;}
        if(!ext2_ready||native_foreign(path)){result=-95;break;}
        u32 atime=native_timestamp(),mtime=atime;
        if(times_address){u64 *times=native_buffer(current_task,times_address,n==132?16:32,0);if(!times){result=-14;break;}
            if(n==132){atime=(u32)times[0];mtime=(u32)times[1];}else{if(times[1]>=1000000||times[3]>=1000000){result=-22;break;}atime=(u32)times[0];mtime=(u32)times[2];}}
        result=-ext4_atime_set(NFILES[index].path,atime);if(!result)result=-ext4_mtime_set(NFILES[index].path,mtime);if(!result)result=-ext4_ctime_set(NFILES[index].path,native_timestamp());break;}
    case 97:case 302:{u64 target=n==97?b:d;u64 resource=n==97?a:b;if(resource>=16){result=-22;break;}
        if(n==302&&c&&!native_buffer(current_task,c,16,0)){result=-14;break;}
        if(target){u64 *out=native_buffer(current_task,target,16,1);if(!out){result=-14;break;}out[0]=out[1]=resource==3?0x200000:resource==7?NATIVE_FDS:~0ULL;}result=0;break;}
    case 98:{void *out=native_buffer(current_task,b,144,1);if(!out)result=-14;else{memset(out,0,144);result=0;}break;}
    case 99:{u8 *out=native_buffer(current_task,a,112,1);if(!out){result=-14;break;}memset(out,0,112);*(u64 *)out=timer_ticks/100;
        *(u64 *)(out+32)=NATIVE_SIZE;*(u64 *)(out+40)=NATIVE_SIZE/2;*(u32 *)(out+104)=1;result=0;break;}
    case 102:case 104:case 107:case 108:result=1000;break;
    case 110:result=p->parent>=0?p->parent+100:1;break;
    case 109:case 111:case 121:case 124:{u64 id=(n==111||!a)?current_task:a>=100?a-100:TASK_COUNT;
        if(id>=TASK_COUNT||!native_active[id]||tasks[id].state==DEAD){result=-3;break;}
        if(n==109){if(id!=current_task&&native_process[id].parent!=(int)current_task){result=-3;break;}native_process[id].pgid=b?b:id+100;result=0;}
        else result=n==124?native_process[id].sid:native_process[id].pgid;break;}
    case 112:p->sid=p->pgid=current_task+100;result=p->sid;break;
    case 158:if((a==0x1002||a==0x1001)&&(!b||(b>=USER_BASE&&b<NATIVE_END))){if(a==0x1002)task_fsbase[current_task]=b;else task_gsbase[current_task]=b;result=0;}
        else if(a==0x1003||a==0x1004){u64 *out=native_buffer(current_task,b,8,1);if(!out)result=-14;else{*out=a==0x1003?task_fsbase[current_task]:task_gsbase[current_task];result=0;}}else result=-22;break;
    case 203:case 204:{u64 id=a?a-100:current_task;
        if(id<APP_FIRST||id>=TASK_COUNT||!native_active[id]||tasks[id].state==DEAD){result=-3;break;}
        if(!b||b>4096||(n==204&&b<8)){result=-22;break;}u8 *mask=native_buffer(current_task,c,b,n==204);if(!mask){result=-14;break;}
        if(n==204){memset(mask,0,b);mask[0]=(u8)task_affinity[id];result=8;}
        else{u32 selected=mask[0]&((1U<<cpu_count)-1);if(!selected)result=-22;else{task_affinity[id]=selected;result=0;}}break;}
    case 309:{u32 *cpu=a?native_buffer(current_task,a,4,1):0,*node=b?native_buffer(current_task,b,4,1):0;
        if((a&&!cpu)||(b&&!node))result=-14;else{if(cpu)*cpu=cpu_local()->index;if(node)*node=0;result=0;}break;}
    case 202:result=native_futex_call(a,b,c,d,e,g);break;
    case 218:p->clear_tid=a;result=current_task+100;break;
    case 221:result=a<NATIVE_FDS&&p->fd[a].kind?0:-9;break; /* fadvise is advisory */
    case 217:result=native_getdents(a,b,c);break;
    case 228:case 96:{if(n==228&&a!=0&&a!=1&&a!=4&&a!=5&&a!=6&&a!=7){result=-22;break;}u64 address=n==228?b:a;u64 *out=native_buffer(current_task,address,16,1);if(!out){result=-14;break;}out[0]=timer_ticks/100+((n==96||a==0||a==5)?native_epoch_base:0);out[1]=(timer_ticks%100)*(n==228?10000000:10000);result=0;break;}
    case 280:{int index;if(d&~256ULL){result=-22;break;}
        if(!b){if(a>=NATIVE_FDS||p->fd[a].kind!=1){result=-9;break;}index=p->fd[a].index;ns_copy(path,NFILES[index].path);}
        else{result=native_at_path((i64)a,b,path);if(result)break;index=native_find(path);if(index<0){result=-2;break;}}
        if(!ext2_ready||native_foreign(path)){result=-95;break;}u64 *times=c?native_buffer(current_task,c,32,0):0;if(c&&!times){result=-14;break;}
        if(times){int invalid=0;for(int i=0;i<4;i+=2)if(times[i+1]!=1073741823&&times[i+1]!=1073741822){if(times[i+1]>=1000000000)invalid=22;else if(times[i]>0xffffffffULL)invalid=75;}if(invalid){result=-invalid;break;}}
        u32 atime=native_timestamp(),mtime=atime;if(times){if(times[1]!=1073741823)atime=times[0];if(times[3]!=1073741823)mtime=times[2];}
        result=0;if(!times||times[1]!=1073741822)result=-ext4_atime_set(NFILES[index].path,atime);
        if(!result&&(!times||times[3]!=1073741822))result=-ext4_mtime_set(NFILES[index].path,mtime);
        if(!result)result=-ext4_ctime_set(NFILES[index].path,native_timestamp());break;}
    case 273:if(b!=24)result=-22;else{p->robust_head=a;result=0;}break;
    case 274:{u64 id=a?a-100:current_task;
        if(id<APP_FIRST||id>=TASK_COUNT||!native_active[id]||tasks[id].state==DEAD){result=-3;break;}
        u64 *head=native_buffer(current_task,b,8,1),*length=native_buffer(current_task,c,8,1);
        if(!head||!length){result=-14;break;}*head=native_process[id].robust_head;*length=24;result=0;break;}
    case 318:{if(c&~3ULL){result=-22;break;}if(!b){result=0;break;}if(b>256)b=256;u8 *out=native_buffer(current_task,a,b,1);result=out?entropy_fill(out,b):-14;break;}
    default:serial("NATIVE unsupported syscall=");hex(n);serial("\r\n");break;
    }
    if(result==-4096){waiting->rip=f->rip;tasks[current_task].frame.rip-=2;tasks[current_task].state=WAIT_EVENT;return schedule();}
    if(result==-11&&(n==61||n==247||((n==0||n==1||n==19||n==20)&&a<NATIVE_FDS&&!(native_descriptions[p->fd[a].description].flags&0x800)))){
        native_wait_blocks++;
        native_wait_reset(current_task);*waiting=(NativeWait){.kind=(n==61||n==247)?2:1,.syscall=n,.address=a,.deadline=~0ULL,.count=n==61?c|1:n==247?d|1:(n==1||n==20)?4:1,.rip=f->rip};
        tasks[current_task].frame.rip-=2;tasks[current_task].state=WAIT_EVENT;return schedule();}
    if(waiting->kind&&waiting->syscall==n&&(!waiting->rip||waiting->rip==f->rip))native_wait_reset(current_task);
    tasks[current_task].frame.rax=result;
    return schedule();
}
