/* Linux-compatible thread entry over Aurora tasks. VM, descriptors and signal
 * dispositions are shared explicitly; scheduling/TLS remain per task. */
static void native_futex_notify(u32 id,u64 address){
    if(address<USER_BASE||address>=NATIVE_END)return;
    u64 physical=(native_alias_pt(id)[(address-USER_BASE)/4096]&0x000ffffffffff000ULL)|(address&4095);
    u64 private=((u64)native_space(id)<<32)|address;
    for(u32 task=APP_FIRST;task<TASK_COUNT;task++){NativeWait *w=&native_waits[task];
        if(tasks[task].state==WAIT_EVENT&&w->kind==6&&(w->key==physical||w->key==private)){
            w->awoken=1;tasks[task].state=RUNNABLE;native_wait_wakes++;}}
}
static void native_robust_release(u32 id,u64 node,i64 offset){
    u64 address=node+offset;if(address&3)return;u32 *word=native_buffer(id,address,4,1);
    if(word&&(*word&0x3fffffffU)==id+100){*word=(*word&0x80000000U)|0x40000000U;native_futex_notify(id,address);}
}
static void native_thread_exit(u32 id){
    NativeProcess *p=&native_process[id];if(!native_vm_attached[id])return;
    if(p->robust_head){u64 *head=native_buffer(id,p->robust_head,24,0);if(head){
        u64 node=head[0],pending=head[2];i64 offset=head[1];
        for(int count=0;count<2048&&node&&node!=p->robust_head;count++){
            u64 *next=native_buffer(id,node,8,0);if(!next)break;u64 following=*next;
            native_robust_release(id,node,offset);node=following;}
        if(pending)native_robust_release(id,pending,offset);}}
    if(p->clear_tid){u32 *word=native_buffer(id,p->clear_tid,4,1);if(word){*word=0;native_futex_notify(id,p->clear_tid);}}
    p->clear_tid=p->robust_head=0;
}
static i64 native_clone_thread(Frame *frame,u64 flags,u64 stack,u64 parent_tid,u64 child_tid,u64 tls){
    const u64 required=0x10f00,allowed=0x17d0f00;
    if((flags&required)!=required||(flags&~allowed)||!stack)return -22;
    if(!native_buffer(current_task,stack-16,16,1))return -14;
    u32 *ptid=(flags&0x100000)?native_buffer(current_task,parent_tid,4,1):0;
    u32 *ctid=(flags&(0x1000000|0x200000))?native_buffer(current_task,child_tid,4,1):0;
    if(((flags&0x100000)&&!ptid)||((flags&(0x1000000|0x200000))&&!ctid))return -14;
    if((flags&0x80000)&&!native_buffer(current_task,tls,1,0))return -14;
    int id=native_slot();if(id<0)return -11;u32 parent=current_task;native_wait_reset(id);
    native_process[id]=native_process[parent];task_affinity[id]=task_affinity[parent];NativeProcess *p=&native_process[id];
    p->thread=1;p->reaped=0;p->vfork_parent=-1;p->clear_tid=(flags&0x200000)?child_tid:0;p->robust_head=0;
    native_fd_users[p->fd_owner]++;native_group_refs[p->tgid-100]++;
    native_vm_owner[id]=native_space(parent);native_vm_refs[native_vm_owner[id]]++;native_vm_attached[id]=1;
    task_cr3[id]=task_cr3[parent];native_active[id]=1;
    memset(native_signals(id),0,sizeof(NativeSignals));
    memset(&tasks[id],0,sizeof(Task));tasks[id].frame=*frame;tasks[id].frame.rax=0;tasks[id].frame.rsp=stack;
    task_fsbase[id]=(flags&0x80000)?tls:task_fsbase[parent];task_gsbase[id]=task_gsbase[parent];memcpy(task_fp[id],task_fp[parent],512);
    task_faults[id]=0;exit_codes[id]=0;
    if(ptid)*ptid=id+100;if((flags&0x1000000)&&ctid)*ctid=id+100;return id+100;
}
