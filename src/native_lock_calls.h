static i64 native_lock_call(u64 syscall,u64 fd,u64 command,u64 address){
    NativeProcess *p=&native_process[current_task];
    if(fd>=NATIVE_FDS||p->fd[fd].kind!=1)return -9;
    NativeFd *f=&p->fd[fd];NativeFile *file=&NFILES[f->index];
    if(!ext2_ready||native_foreign(file->path)||file->kind!=1)return -95;
    u64 key=*(u32 *)(file->pad+16);if(!key)return -95;
    u32 kind=syscall==73?2:1,owner=kind==2?f->description:p->tgid,type,blocking;
    u8 *request=0;
    if(kind==2){
        u64 op=command&~4ULL;if(command&~15ULL||!(op==1||op==2||op==8))return -22;
        type=op==8?0:(u32)op;blocking=!(command&4);
        /* Linux flock conversions release the old lock before reacquiring. */
        native_lock_release(key,owner,kind);
    }else{
        request=native_buffer(current_task,address,32,command==5);if(!request)return -14;
        u16 mode=*(u16 *)request;if(mode>2)return -22;
        /* Never pretend to implement byte ranges: dpkg uses whole-file locks. */
        if(*(u16 *)(request+2)||*(u64 *)(request+8)||*(u64 *)(request+16))return -95;
        type=mode==2?0:mode+1;blocking=command==7;
        if(command==5){
            if(!type)return -22;NativeLock *conflict=native_lock_conflict(key,owner,kind,type);
            *(u16 *)request=conflict?conflict->type-1:2;
            if(conflict){*(u32 *)(request+24)=conflict->owner;*(u64 *)(request+8)=*(u64 *)(request+16)=0;}return 0;
        }
        u32 access=native_descriptions[f->description].flags&3;
        if((type==1&&access==1)||(type==2&&!access))return -9;
    }
    int result=native_lock_set(key,owner,kind,type);
    if(result!=-11||!blocking)return result;
    native_waits[current_task]=(NativeWait){.kind=9,.syscall=syscall,.key=key,.count=owner,.extra={kind,type},.deadline=~0ULL};
    native_wait_blocks++;return -4096;
}
