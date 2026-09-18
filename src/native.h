/* Limited Linux x86-64 ABI personality for the pinned static GCC/musl tools.
 * This is Aurora code: no Linux kernel or host-side compilation service runs. */
#include "rtc_time.h"
#include "virtio_block.h"
#include "virtio_net.h"
#include "native_fs.h"
#define NATIVE_SIZE 0x20000000ULL
#define NATIVE_MMAP_BASE 0x10000000ULL
#define NATIVE_COW 512ULL
#define NATIVE_SHARED 1024ULL
#define NATIVE_END (USER_BASE+NATIVE_SIZE)
#define NATIVE_STACK (NATIVE_END-0x200000)
#define NATIVE_SLOTS (TASK_COUNT-APP_FIRST)
#define NATIVE_FDS 64
/* Descriptor flags are per fd; offsets and status flags belong to the shared
 * open description, including across fork and dup. Zero is never allocated. */
typedef struct {u64 offset;u32 flags,refs;} NativeDescription;
static NativeDescription native_descriptions[257];
typedef struct {int kind,index;u32 description,flags;} NativeFd;
static int native_description(u32 flags){
    for(int i=1;i<257;i++)if(!native_descriptions[i].refs){
        native_descriptions[i]=(NativeDescription){.flags=flags&~0x80000U,.refs=1};return i;
    }
    return 0;
}
typedef struct {
    NativeFd *fd;u64 brk,min_brk,map_next;int parent,vfork_parent,reaped;
    char cwd[256],exe[256];u64 sigmask;u32 umask;int pgid,sid,stopped_signal;
    u32 fd_owner,signal_owner,fs_owner,tgid;u64 clear_tid,robust_head;int thread;
} NativeProcess;
static NativeProcess native_process[TASK_COUNT];
static NativeFd native_fd_tables[TASK_COUNT][NATIVE_FDS];
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
typedef struct {NativeSigaction action[65];Frame saved;u64 mask,pending;int active;u8 fp[512];u64 alt_sp,alt_size;int on_alt;} NativeSignals;
_Static_assert(sizeof(NativeSignals)<=4096,"signal state must fit reserved page");
static NativeSignals *native_signals(u32 id){return (NativeSignals *)(0x0c000000ULL+id*4096);}
static NativeSigaction *native_actions(u32 id){return native_signals(native_process[id].signal_owner)->action;}
#define NATIVE_PIPE_CAPACITY 4096
typedef struct {u8 bytes[NATIVE_PIPE_CAPACITY];u32 size,readers,writers;} NativePipe;
#define native_pipes ((NativePipe *)0x0c300000)
#define NATIVE_EXEC_ARGS 4096
#define NATIVE_EXEC_BYTES 0x100000
#define exec_strings ((char *)0x0c400000)
#define exec_args ((char **)0x0c600000)
#define exec_argv ((u64 *)0x0c610000)
#define exec_env ((char (*)[4096])0x0c500000)
static int exec_argc,exec_envc;
/* Supervisor-only aliases mirror the process mappings. Physical pages come
 * from BIOS-reported RAM; no per-process contiguous reservation is needed. */
#define NATIVE_PAGE_FIRST 0x10000000ULL
#define NATIVE_PAGE_LIMIT 0x40000000ULL
#define NATIVE_PAGE_COUNT ((NATIVE_PAGE_LIMIT-NATIVE_PAGE_FIRST)/4096)
#define NATIVE_PAGE_BITMAP ((u8 *)0x08800000)
#define NATIVE_PAGE_REFS ((u16 *)0x08810000)
#define NATIVE_ALIAS_PD ((u64 *)0x08700000)
static u32 native_free_pages;
static u32 native_page_hint;
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
static u64 native_phys(u32 id){return 0x100000000ULL+(u64)(native_space(id)-APP_FIRST)*NATIVE_SIZE;}
static u64 *native_pt(u32 id){return (u64 *)(task_cr3[id]+0x6000);}
static u64 *native_alias_pt(u32 id){return (u64 *)(0x09000000ULL+(native_space(id)-APP_FIRST)*0x100000ULL);}
static void native_memory_init(void){
    if(!native_ready)return;
    memset(native_pipes,0,16*sizeof(NativePipe));
    memset(NATIVE_PAGE_BITMAP,0xff,NATIVE_PAGE_COUNT/8);memset(NATIVE_PAGE_REFS,0,NATIVE_PAGE_COUNT*2);memset(NATIVE_ALIAS_PD,0,32768);
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
    for(int i=0;i<7;i++)((u64 *)0x201000)[4+i]=(0x08700000ULL+i*4096)|3;
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
static void native_unmap_page(u32 id,u64 page){
    native_vm_barrier(id);
    u64 *pt=native_pt(id);if(native_alias_pt(id)[page]&1)native_page_release(native_alias_pt(id)[page]&0x000ffffffffff000ULL);pt[page]=0;
    native_alias_pt(id)[page]=0;
    u64 alias=native_phys(id)+page*4096;__asm__ volatile("invlpg (%0)"::"r"(alias):"memory");
}
static void native_memory_release(u32 id){
    if(!native_vm_attached[id])return;u32 owner=native_vm_owner[id];
    if(!--native_vm_refs[owner])for(u64 i=0;i<NATIVE_SIZE/4096;i++)if(native_alias_pt(id)[i]&1)native_unmap_page(id,i);
    native_vm_attached[id]=0;
}
static void *native_buffer(u32 id,u64 address,u64 size,int write){
    if(!size||address<USER_BASE||address>=NATIVE_END||size>NATIVE_END-address)return 0;
    u64 *pt=native_pt(id);
    for(u64 p=(address-USER_BASE)/4096;p<=(address+size-1-USER_BASE)/4096;p++){
        if((pt[p]&5)!=5||(write&&!(pt[p]&(WRITE|NATIVE_COW))))return 0;
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
    u64 root=0x0a000000ULL+(id-APP_FIRST)*0x110000;task_cr3[id]=root;memset((void *)root,0,0x110000);
    u64 *pml4=(u64 *)root,*pdpt=(u64 *)(root+0x1000),*pd=(u64 *)(root+0x2000);
    pml4[0]=(root+0x1000)|7;for(int i=0;i<4;i++)pdpt[i]=(root+0x2000+i*4096)|7;
    for(int i=0;i<2048;i++)pd[i]=(u64)i*0x200000|PRESENT|WRITE|HUGE;
    pd[0]=0x207003;pd[1]=0x208003;
    memset(native_alias_pt(id),0,0x100000);
    for(int i=0;i<256;i++){pd[i+2]=(root+0x6000+i*4096)|7;NATIVE_ALIAS_PD[(id-APP_FIRST)*256+i]=((u64)native_alias_pt(id)+i*4096)|3;}
    native_active[id]=1;
}
static int native_map(u32 id,u64 address,u64 size,u64 flags){
    native_vm_barrier(id);
    if(!size||address<USER_BASE||address>=NATIVE_END||size>NATIVE_END-address)return 0;
    u64 *pt=native_pt(id);
    u64 needed=0;for(u64 page=(address-USER_BASE)/4096;page<=(address+size-1-USER_BASE)/4096;page++)if(!(native_alias_pt(id)[page]&1)||NATIVE_PAGE_REFS[((native_alias_pt(id)[page]&0x000ffffffffff000ULL)-NATIVE_PAGE_FIRST)/4096]>1)needed++;
    if(needed>native_free_pages)return 0;
    for(u64 page=(address-USER_BASE)/4096;page<=(address+size-1-USER_BASE)/4096;page++){
        if((native_alias_pt(id)[page]&1)&&!native_private_page(id,page))return 0;
        u64 physical=(native_alias_pt(id)[page]&1)?native_alias_pt(id)[page]&0x000ffffffffff000ULL:native_page_allocate();
        pt[page]=physical|PRESENT|USER|flags;
        native_alias_pt(id)[page]=physical|PRESENT|WRITE|NX;
        u64 alias=native_phys(id)+page*4096;__asm__ volatile("invlpg (%0)"::"r"(alias):"memory");
    }return 1;
}
/* Search mapped pages instead of consuming the mmap arena monotonically. */
static u64 native_mapping_gap(u32 id,u64 size){
    u64 run=0,*pt=native_alias_pt(id);
    for(u64 address=NATIVE_MMAP_BASE;address<NATIVE_STACK-4096;address+=4096){
        if(pt[(address-USER_BASE)/4096]&1)run=0;else run+=4096;
        if(run>=size)return address+4096-size;
    }
    return 0;
}
static void native_close(u32 id,int fd){
    NativeFd *f=&native_process[id].fd[fd];
    int file_index=f->kind==1?f->index:-1;
    if(f->description&&native_descriptions[f->description].refs)native_descriptions[f->description].refs--;
    if(f->kind==6&&!native_descriptions[f->description].refs)network_close(f->index);
    if(f->kind==2&&native_pipes[f->index].readers)native_pipes[f->index].readers--;
    if(f->kind==3&&native_pipes[f->index].writers)native_pipes[f->index].writers--;
    memset(f,0,sizeof(*f));
    if(file_index>=0&&ext2_ready)vfs_close_deleted(file_index);
}
static void native_finish(u32 id,i64 code){
    native_wait_reset(id);
    NativeProcess *p=&native_process[id];native_thread_exit(id);
    if(p->fd&&native_fd_users[p->fd_owner]&&!--native_fd_users[p->fd_owner])for(int i=0;i<NATIVE_FDS;i++)native_close(id,i);
    native_memory_release(id);
    task_kernel_sp[id]=0;if(p->thread)p->reaped=1;
    tasks[id].state=DEAD;exit_codes[id]=code;
    u32 leader=p->tgid>=100?p->tgid-100:id;
    if(leader<TASK_COUNT&&native_group_refs[leader]&&!--native_group_refs[leader]){int parent=native_process[leader].parent;
        exit_codes[leader]=code;
        if(parent>=0&&native_active[parent])native_signals(parent)->pending|=1ULL<<16;}
    if(leader<TASK_COUNT&&!native_group_refs[leader])for(u32 child=APP_FIRST;child<TASK_COUNT;child++)if(native_active[child]&&native_process[child].parent==(int)leader)native_process[child].parent=-1;
    if(p->vfork_parent>=0){tasks[p->vfork_parent].state=RUNNABLE;p->vfork_parent=-1;}
}
static void native_signal_queue(u32 id,u32 signal){
    if(task_kernel_sp[id]){native_signals(id)->pending|=1ULL<<(signal-1);return;}
    if(signal==9){native_mark_group(id,-9);return;}
    if(signal==19){tasks[id].state=4;native_process[id].stopped_signal=19;if(native_process[id].parent>=0)native_signals(native_process[id].parent)->pending|=1ULL<<16;return;}
    if(signal==18&&tasks[id].state==4)tasks[id].state=RUNNABLE;
    native_signals(id)->pending|=1ULL<<(signal-1);
}
static void native_signal_deliver(u32 id){
    if(!native_active[id])return;NativeSignals *s=native_signals(id);NativeProcess *p=&native_process[id];Frame *f=&tasks[id].frame;
    if(s->active&&!s->on_alt&&f->rsp>=s->saved.rsp-128)s->active=0;
    if(s->active)return;
    u64 pending=s->pending&~p->sigmask;if(!pending)return;
    u32 signal=1;while(!(pending&1)){signal++;pending>>=1;}s->pending&=~(1ULL<<(signal-1));
    NativeSigaction action=native_actions(id)[signal];if(action.handler==1)return;
    if(!action.handler){if(signal==17||signal==18||signal==23||signal==28)return;
        if(signal==20||signal==21||signal==22){tasks[id].state=4;p->stopped_signal=signal;if(p->parent>=0)native_signals(p->parent)->pending|=1ULL<<16;return;}
        native_mark_group(id,-(i64)signal);return;}
    if(!action.restorer||!native_buffer(id,action.handler,1,0)||!native_buffer(id,action.restorer,1,0)){native_mark_group(id,-11);return;}
    u64 top=f->rsp;
    if((action.flags&0x08000000)&&s->alt_size&&!s->on_alt){top=s->alt_sp+s->alt_size;s->on_alt=1;}
    if(top<USER_BASE+1672){native_mark_group(id,-11);return;}
    u64 base=(top-128-1536)&~15ULL,sp=base-8;
    u8 *buffer=native_buffer(id,sp,1544,1);if(!buffer){native_mark_group(id,-11);return;}
    memset(buffer,0,1544);*(u64 *)buffer=action.restorer;*(u32 *)(buffer+8)=signal;
    u64 *context=(u64 *)(buffer+8+128+40);
    u64 regs[]={f->r8,f->r9,f->r10,f->r11,f->r12,f->r13,f->r14,f->r15,f->rdi,f->rsi,f->rbp,f->rbx,f->rdx,f->rax,f->rcx,f->rsp,f->rip,f->flags,0x001b000000000023ULL,0,0,p->sigmask,0,base+512};
    memcpy(context,regs,sizeof(regs));*(u64 *)(buffer+8+128+296)=p->sigmask;
    memcpy(buffer+8+512,task_fp[id],512);memcpy(s->fp,task_fp[id],512);s->saved=*f;s->mask=p->sigmask;s->active=1;
    p->sigmask|=action.mask;if(!(action.flags&0x40000000))p->sigmask|=1ULL<<(signal-1);
    if(action.flags&0x80000000)native_actions(id)[signal].handler=0;
    f->rsp=sp;f->rip=action.handler;f->rdi=signal;f->rsi=base;f->rdx=base+128;f->rax=0;
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
    u64 needed=512+main.pages+(loader>=0?interpreter.pages:0),available=native_free_pages;
    if(native_vm_attached[id]&&native_vm_refs[native_space(id)]==1)for(u64 i=0;i<NATIVE_SIZE/4096;i++)if((native_alias_pt(id)[i]&1)&&NATIVE_PAGE_REFS[((native_alias_pt(id)[i]&0x000ffffffffff000ULL)-NATIVE_PAGE_FIRST)/4096]==1)available++;
    if(needed>available)return -12;
    native_tables(id);
    error=native_elf_map(id,index,&main);
    if(!error&&loader>=0)error=native_elf_map(id,loader,&interpreter);
    if(error){native_finish(id,127);return error;}
    if(!native_map(id,NATIVE_STACK,0x200000,WRITE|NX)){native_finish(id,127);return -12;}
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
    NativeSignals *signals=native_signals(id);memcpy(signals->action,native_actions(id),sizeof(signals->action));p->signal_owner=id;signals->active=signals->on_alt=0;signals->alt_sp=signals->alt_size=0;
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
static i64 native_shell_file(u64 address,int write){
    FileRequest *r=user_buffer(current_task,address,sizeof(FileRequest),0);if(!r)return ERR_POINTER;
    if(!native_ready)return ERR_NOT_FOUND;if(!name_valid(r->name)||r->size>FS_MAX_SIZE)return ERR_NAME;
    void *buffer=r->size?user_buffer(current_task,r->buffer,r->size,!write):0;if(r->size&&!buffer)return ERR_POINTER;
    char path[256];native_path(path,"/work",r->name);int index=native_find(path);
    if(index<0&&write)index=native_create(path);if(index<0)return ERR_NOT_FOUND;
    if(write){NFILES[index].size=0;return native_write(index,0,buffer,r->size);}
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
    if(ns_equal(path,"/")||ns_equal(path,"/exchange"))return -16;
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
    if(f->kind==8){
        if(write){u64 value=*(u64 *)buffer;if(value==~0ULL)return -22;if(value>~1ULL-of->offset)return -11;of->offset+=value;}
        else{if(!of->offset)return -11;*(u64 *)buffer=f->index?1:of->offset;if(f->index)of->offset--;else of->offset=0;}
        return 8;
    }
    if(f->kind==4){if(write)return native_console(buffer,size);NativeTty *tty=NATIVE_TTY;
        if((int)tty->foreground!=native_process[current_task].pgid){native_signal_queue(current_task,21);return -11;}
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
static i64 native_stat(int index,u64 address,int terminal){
    if(!terminal&&ns_equal(NFILES[index].path,"/dev/null"))terminal=1;
    u8 *out=native_buffer(current_task,address,144,1);if(!out)return -14;memset(out,0,144);
    *(u64 *)(out)=1;*(u64 *)(out+8)=index+1;*(u64 *)(out+16)=1;
    u32 mode=terminal?0666:(*(u32 *)(NFILES[index].pad+4)?*(u32 *)NFILES[index].pad:0755);
    *(u32 *)(out+24)=(terminal?(terminal==6?0140000:0020000):(NFILES[index].kind==2?0040000:NFILES[index].kind==3?0120000:0100000))|mode;
    *(u32 *)(out+28)=1000;*(u32 *)(out+32)=1000;
    *(u64 *)(out+48)=terminal?0:NFILES[index].size;*(u64 *)(out+56)=4096;*(u64 *)(out+64)=terminal?0:(NFILES[index].size+511)/512;
    if(!terminal&&ext2_ready&&!(fat_ready&&fat_path(NFILES[index].path))){
        uint32_t value=0;ext4_atime_get(NFILES[index].path,&value);*(u64 *)(out+72)=value;
        ext4_mtime_get(NFILES[index].path,&value);*(u64 *)(out+88)=value;ext4_ctime_get(NFILES[index].path,&value);*(u64 *)(out+104)=value;
    }
    return 0;
}
static i64 native_fork(Frame *frame,int vfork,u64 child_stack){
    native_vm_barrier(current_task);
    int id=native_slot();if(id<0)return -11;u32 parent=current_task;
    native_wait_reset(id);
    u64 *source=native_pt(parent);
    if(vfork){native_vm_owner[id]=native_space(parent);native_vm_refs[native_vm_owner[id]]++;native_vm_attached[id]=1;task_cr3[id]=task_cr3[parent];native_active[id]=1;}
    else{native_tables(id);
    for(u64 i=0;i<NATIVE_SIZE/4096;i++)if(native_alias_pt(parent)[i]&1){
        u64 physical=native_alias_pt(parent)[i]&0x000ffffffffff000ULL;
        NATIVE_PAGE_REFS[(physical-NATIVE_PAGE_FIRST)/4096]++;
        if((source[i]&WRITE)&&!(source[i]&NATIVE_SHARED))source[i]=(source[i]&~WRITE)|NATIVE_COW;
        native_pt(id)[i]=source[i];native_alias_pt(id)[i]=native_alias_pt(parent)[i];
    }}
    native_process[id]=native_process[parent];task_affinity[id]=task_affinity[parent];NativeProcess *p=&native_process[id];p->parent=native_process[parent].tgid-100;p->vfork_parent=vfork?(int)parent:-1;p->reaped=0;
    p->fd=native_fd_tables[id];memcpy(p->fd,native_process[parent].fd,sizeof(native_fd_tables[id]));p->fd_owner=p->fs_owner=p->signal_owner=id;native_fd_users[id]=1;
    p->tgid=id+100;native_group_refs[id]=1;p->thread=0;p->clear_tid=p->robust_head=0;p->brk=native_process[native_space(parent)].brk;
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
static Frame *native_dispatch(Frame *f){
    u64 n=f->rax,a=f->rdi,b=f->rsi,c=f->rdx,d=f->r10,e=f->r8,g=f->r9;
    NativeProcess *p=&native_process[current_task];i64 result=-38;char path[256];
    NativeWait *waiting=&native_waits[current_task];
    if(waiting->kind&&waiting->syscall==n&&!native_signals(current_task)->active&&waiting->interrupted){
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
        if(ext2_ready&&!(fat_ready&&fat_path(path))&&(n==6||(n==262&&(d&256)))){char resolved[256];result=ext2_resolve(resolved,path,0);if(result)break;
            index=-1;for(u32 i=0;i<native_count;i++)if(NFILES[i].kind&&ns_equal(NFILES[i].path,resolved)){index=i;break;}if(index<0)index=ext2_find(resolved);
        }else index=native_find(path);result=index<0?-2:native_stat(index,target,0);break;}
    case 5:result=a>=NATIVE_FDS||!p->fd[a].kind?-9:native_stat(p->fd[a].index,b,p->fd[a].kind==6?6:p->fd[a].kind!=1);break;
    case 8:if(a>=NATIVE_FDS||!p->fd[a].kind){result=-9;break;}if(p->fd[a].kind==5){result=c>4?-22:0;break;}if(p->fd[a].kind!=1){result=-29;break;}
        {i64 position=(i64)b;if(c==1)position+=(i64)native_descriptions[p->fd[a].description].offset;else if(c==2)position+=(i64)NFILES[p->fd[a].index].size;else if(c!=0){result=-22;break;}
        if(position<0)result=-22;else result=native_descriptions[p->fd[a].description].offset=(u64)position;}break;
    case 9:{u64 size=(b+4095)&~4095ULL;if(!b||b>NATIVE_SIZE||!size||(c&6)==6){result=-22;break;}
        if((d&3)!=1&&(d&3)!=2){result=-22;break;}if((d&1)&&!(d&32)){result=-95;break;}
        u64 address=a;if(!(d&16))address=native_mapping_gap(current_task,size);
        if(address&4095){result=-22;break;}
        if(address<USER_BASE||address>=NATIVE_STACK-4096||size>NATIVE_STACK-4096-address){result=-12;break;}
        if(!(d&32)&&(e>=NATIVE_FDS||p->fd[e].kind!=1)){result=-9;break;}
        if(!native_map(current_task,address,size,((c&2)?WRITE:0)|((c&4)?0:NX)|((d&1)?NATIVE_SHARED:0))){result=-12;break;}
        if(d&16)memset((void *)(native_phys(current_task)+address-USER_BASE),0,size);
        if(!c)for(u64 i=(address-USER_BASE)/4096;i<(address+size-USER_BASE)/4096;i++)native_pt(current_task)[i]&=~1ULL;
        if(!(d&32)){result=native_read(p->fd[e].index,g,(void *)(native_phys(current_task)+address-USER_BASE),b);if(result<0)break;}
        result=address;break;}
    case 10:native_vm_barrier(current_task);if((a&4095)||a<USER_BASE||!b||a>=NATIVE_END||b>NATIVE_END-a||(c&6)==6){result=-22;break;}
        result=0;for(u64 i=(a-USER_BASE)/4096;i<=(a+b-1-USER_BASE)/4096;i++)if(!(native_alias_pt(current_task)[i]&1))result=-12;
        if(result)break;for(u64 i=(a-USER_BASE)/4096;i<=(a+b-1-USER_BASE)/4096;i++){u64 *pt=native_pt(current_task);u64 physical=pt[i]&0x000ffffffffff000ULL;u64 writable=(c&2)?(NATIVE_PAGE_REFS[(physical-NATIVE_PAGE_FIRST)/4096]>1&&!(pt[i]&NATIVE_SHARED)?NATIVE_COW:WRITE):0;pt[i]=physical|(pt[i]&NATIVE_SHARED)|(c?PRESENT:0)|USER|writable|((c&4)?0:NX);}break;
    case 11:if((a&4095)||a<USER_BASE||!b||a>=NATIVE_END||b>NATIVE_END-a){result=-22;break;}
        for(u64 i=(a-USER_BASE)/4096;i<=(a+b-1-USER_BASE)/4096;i++)native_unmap_page(current_task,i);result=0;break;
    case 12:{NativeProcess *vm=&native_process[native_space(current_task)];
        if(a>=vm->min_brk&&a<NATIVE_MMAP_BASE-4096){
            if(a>vm->brk){if(!native_map(current_task,vm->brk,a-vm->brk,WRITE|NX)){result=vm->brk;break;}}
            else for(u64 address=(a+4095)&~4095ULL;address<vm->brk;address+=4096)native_unmap_page(current_task,(address-USER_BASE)/4096);
            vm->brk=a;}result=vm->brk;break;}
    case 13:{if(!a||a>64||d!=8||(b&&(a==9||a==19))){result=-22;break;}NativeSigaction *in=b?native_buffer(current_task,b,32,0):0,*out=c?native_buffer(current_task,c,32,1):0;
        if((b&&!in)||(c&&!out)){result=-14;break;}NativeSigaction next;if(in)next=*in;if(out)*out=native_actions(current_task)[a];if(in)native_actions(current_task)[a]=next;result=0;break;}
    case 15:{NativeSignals *s=native_signals(current_task);if(!s->active){native_finish(current_task,-11);return schedule();}
        tasks[current_task].frame=s->saved;p->sigmask=s->mask;memcpy(task_fp[current_task],s->fp,512);s->active=s->on_alt=0;return schedule();}
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
    case 17:if(a>=NATIVE_FDS||p->fd[a].kind!=1){result=-9;break;}{void *out=c?native_buffer(current_task,b,c,1):0;result=c&&!out?-14:native_read(p->fd[a].index,d,out,c);}break;
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
        result=0;for(u64 i=0;i<count;i++)if(!(native_alias_pt(current_task)[(a-USER_BASE)/4096+i]&1)){result=-12;break;}
        if(!result)memset(out,1,count);break;}
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
        if(out){out[0]=s->alt_sp;out[1]=s->on_alt?1:s->alt_size?0:2;out[2]=s->alt_size;}
        result=0;if(in){u32 flags=next[1];if(s->on_alt)result=-1;else if(flags&~2U)result=-22;
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
            if(signal)native_signal_queue(id,signal);result=0;
        }
        break;}
    case 61:{int found=0;result=-10;for(int i=APP_FIRST;i<APP_FIRST+NATIVE_SLOTS;i++)if(native_active[i]&&native_process[i].parent==(int)p->tgid-100&&!native_process[i].thread&&!native_process[i].reaped&&((i64)a==-1||a==(u64)i+100)){
        if(tasks[i].state==4&&(c&2)&&native_process[i].stopped_signal){if(b){int *out=native_buffer(current_task,b,4,1);if(!out){result=-14;break;}*out=(native_process[i].stopped_signal<<8)|127;}
            native_process[i].stopped_signal=0;result=i+100;break;}
        found=1;if(tasks[i].state==DEAD&&!native_group_refs[i]){if(b){int *out=native_buffer(current_task,b,4,1);if(!out){result=-14;break;}*out=exit_codes[i]<0?(exit_codes[i]<=-128?11:(-(int)exit_codes[i]&127)):((int)exit_codes[i]&255)<<8;}
            if(d){void *out=native_buffer(current_task,d,144,1);if(!out){result=-14;break;}memset(out,0,144);}native_process[i].reaped=1;result=i+100;break;}}
        if(found&&result==-10)result=(c&1)?0:-11;break;}
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
        if(fat_ready&&fat_path(path)){FRESULT error=f_mkdir(fat_name(path));result=error?fat_error(error):0;break;}
        result=ext2_ready?-ext4_dir_mk(path):-38;if(!result)result=-ext4_mode_set(path,(n==258?c:b)&0777&~p->umask);break;
    case 84:result=native_user_path(a,path)?native_remove_directory(path):-14;break;
    case 82:case 264:case 316:{char target[256];result=native_at_path(n==82?-100:(i64)a,n==82?a:b,path);if(result)break;
        result=native_at_path(n==82?-100:(i64)c,n==82?b:d,target);if(result)break;
        result=ext2_ready?vfs_rename(path,target,n==316?(u32)e:0):-38;break;}
    case 88:case 266:if(!ext2_ready)result=-38;else{char target[256];
        if(!native_string(a,target,sizeof(target))){result=-14;break;}
        result=native_at_path(n==266?(i64)b:-100,n==266?c:b,path);if(result)break;
        if(fat_ready&&fat_path(path)){result=-95;break;}
        char resolved[256];result=ext2_resolve(resolved,path,0);if(result)break;
        int kind=vfs_kind(resolved);result=kind>=0?-17:kind!=-2?kind:-ext4_fsymlink(target,resolved);}break;
    case 87:case 263:{result=native_at_path(n==263?(i64)a:-100,n==263?b:a,path);if(result)break;
        if(n==263&&c){result=c==512?native_remove_directory(path):-22;break;}
        if(ext2_ready&&!(fat_ready&&fat_path(path))){char resolved[256];result=ext2_resolve(resolved,path,0);if(result)break;uint32_t mode;result=-ext4_mode_get(resolved,&mode);if(result)break;
            if((mode&0170000)==0040000){result=-21;break;}result=vfs_unlink(resolved);
        }else{int index=native_find(path);if(index<0)result=-2;else if(NFILES[index].kind==2)result=-21;else if(!native_writable(path))result=-30;else{NFILES[index].kind=0;result=native_commit(index)?0:-5;}}break;}
    case 89:if(!native_user_path(a,path))result=-14;else if(ns_equal(path,"/proc/self/exe")){u64 length=ns_length(p->exe);if(length>c)length=c;void *out=native_buffer(current_task,b,length,1);if(!out)result=-14;else{memcpy(out,p->exe,length);result=length;}}
        else if(ext2_ready){void *out=native_buffer(current_task,b,c,1);size_t count=0;if(!out)result=-14;else{int error=ext4_readlink(path,out,c,&count);result=error?-error:(i64)count;}}else result=-22;break;
    case 90:case 91:case 268:case 452:{int index;u32 mode=n==268||n==452?c:b;
        if(n==268||n==452){if(n==452&&d){result=-95;break;}result=native_at_path((i64)a,b,path);if(result)break;index=native_find(path);}
        else if(n==90){if(!native_user_path(a,path)){result=-14;break;}index=native_find(path);}
        else index=a<NATIVE_FDS&&p->fd[a].kind==1?p->fd[a].index:-1;
        if(index<0){result=n==90?-2:-9;break;}if(!native_writable(NFILES[index].path)){result=-30;break;}
        *(u32 *)NFILES[index].pad=mode&0777;*(u32 *)(NFILES[index].pad+4)=1;result=native_commit(index)?0:-5;break;}
    case 95:result=p->umask;for(int i=APP_FIRST;i<TASK_COUNT;i++)if(native_active[i]&&native_process[i].fs_owner==p->fs_owner)native_process[i].umask=(u32)a&0777;break;
    case 92:case 93:case 94:{if((b!=1000&&b!=0xffffffffULL&&b!=~0ULL)||(c!=1000&&c!=0xffffffffULL&&c!=~0ULL)){result=-1;break;}
        if(n==93){if(a>=NATIVE_FDS||p->fd[a].kind!=1){result=-9;break;}ns_copy(path,NFILES[p->fd[a].index].path);}
        else if(!native_user_path(a,path)){result=-14;break;}int index=native_find(path);if(index<0){result=-2;break;}
        result=ext2_ready&&!(fat_ready&&fat_path(path))?-ext4_owner_set(NFILES[index].path,1000,1000):0;break;}
    case 97:case 302:{u64 target=n==97?b:d;u64 resource=n==97?a:b;if(target){u64 *out=native_buffer(current_task,target,16,1);if(!out){result=-14;break;}out[0]=out[1]=resource==3?0x200000:resource==7?NATIVE_FDS:~0ULL;}result=0;break;}
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
        if(!ext2_ready||fat_path(path)){result=-95;break;}u64 *times=c?native_buffer(current_task,c,32,0):0;if(c&&!times){result=-14;break;}
        if(times){int invalid=0;for(int i=0;i<4;i+=2)if(times[i+1]!=1073741823&&times[i+1]!=1073741822){if(times[i+1]>=1000000000)invalid=22;else if(times[i]>0xffffffffULL)invalid=75;}if(invalid){result=-invalid;break;}}
        u32 atime=native_timestamp(),mtime=atime;if(times){if(times[1]!=1073741823)atime=times[0];if(times[3]!=1073741823)mtime=times[2];}
        result=0;if(!times||times[1]!=1073741822)result=-ext4_atime_set(NFILES[index].path,atime);
        if(!result&&(!times||times[3]!=1073741822))result=-ext4_mtime_set(NFILES[index].path,mtime);break;}
    case 273:if(b!=24)result=-22;else{p->robust_head=a;result=0;}break;
    case 274:{u64 id=a?a-100:current_task;
        if(id<APP_FIRST||id>=TASK_COUNT||!native_active[id]||tasks[id].state==DEAD){result=-3;break;}
        u64 *head=native_buffer(current_task,b,8,1),*length=native_buffer(current_task,c,8,1);
        if(!head||!length){result=-14;break;}*head=native_process[id].robust_head;*length=24;result=0;break;}
    case 318:{if(c&~3ULL){result=-22;break;}if(!b){result=0;break;}if(b>256)b=256;u8 *out=native_buffer(current_task,a,b,1);result=out?entropy_fill(out,b):-14;break;}
    default:serial("NATIVE unsupported syscall=");hex(n);serial("\r\n");break;
    }
    if(result==-4096){tasks[current_task].frame.rip-=2;tasks[current_task].state=WAIT_EVENT;return schedule();}
    if(result==-11&&(n==61||((n==0||n==1||n==19||n==20)&&a<NATIVE_FDS&&!(native_descriptions[p->fd[a].description].flags&0x800)))){
        native_wait_blocks++;
        native_wait_reset(current_task);*waiting=(NativeWait){.kind=n==61?2:1,.syscall=n,.address=a,.deadline=~0ULL,.count=(n==1||n==20)?4:1};
        tasks[current_task].frame.rip-=2;tasks[current_task].state=WAIT_EVENT;return schedule();}
    if(waiting->kind&&waiting->syscall==n&&!native_signals(current_task)->active)native_wait_reset(current_task);
    tasks[current_task].frame.rax=result;
    return schedule();
}
