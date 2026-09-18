/* Readiness notifications are kernel mechanisms; no driver protocol runs here.
 * Single CPU: syscall entry and IRQ handling serialize enqueue/check/wake. */
typedef struct {int kind,interrupted,awoken,mask_changed;u64 syscall,deadline,address,count,extra[3],oldmask,key,bits;} NativeWait;
static NativeWait native_waits[TASK_COUNT];
volatile u64 native_wait_blocks,native_wait_wakes;
static void native_wait_reset(u32 id){
    if(native_waits[id].mask_changed)native_process[id].sigmask=native_waits[id].oldmask;
    memset(&native_waits[id],0,sizeof(NativeWait));
}
static u32 native_readiness(u32 id,int fd){
    if(fd<0||fd>=NATIVE_FDS||!native_process[id].fd[fd].kind)return 32;
    NativeFd *f=&native_process[id].fd[fd];
    if(f->kind==2){NativePipe *p=&native_pipes[f->index];return (p->size?1:0)|(!p->writers?16:0);}
    if(f->kind==3){NativePipe *p=&native_pipes[f->index];return !p->readers?8:p->size<256?4:0;}
    u32 mode=native_descriptions[f->description].flags&3;
    if(f->kind==4)return ((mode!=1&&(NATIVE_TTY->ready||NATIVE_TTY->eof))?1:0)|(mode?4:0);
    return (mode!=1?1:0)|(mode?4:0);
}
typedef struct {int fd;u16 events,revents;} NativePollFd;
static int native_poll_scan(u32 id,u64 address,u64 count,int write){
    if(count>1024)return -22;if(!count)return 0;
    NativePollFd *fds=native_buffer(id,address,count*sizeof(*fds),write);if(!fds)return -14;
    int ready=0;for(u64 i=0;i<count;i++){u16 events=fds[i].fd<0?0:native_readiness(id,fds[i].fd)&(fds[i].events|8|16|32);
        if(events)ready++;if(write)fds[i].revents=events;}
    return ready;
}
static int native_select_scan(u32 id,NativeWait *w,int write){
    if(w->count>NATIVE_FDS)return -22;u64 input[3]={0},output[3]={0};
    for(int j=0;j<3;j++)if(w->extra[j]&&w->count){u64 *p=native_buffer(id,w->extra[j],8,write);if(!p)return -14;input[j]=*p;}
    int ready=0;for(u64 fd=0;fd<w->count;fd++){u64 bit=1ULL<<fd;if(!((input[0]|input[1]|input[2])&bit))continue;
        u32 events=native_readiness(id,fd);if(events&32)return -9;
        for(int j=0;j<3;j++)if((input[j]&bit)&&(events&(j==0?1|16|8:j==1?4|8:2))){output[j]|=bit;ready++;}}
    if(write)for(int j=0;j<3;j++)if(w->extra[j]&&w->count)*(u64 *)native_buffer(id,w->extra[j],8,1)=output[j];
    return ready;
}
static int native_deadline(u64 address,int absolute,int realtime,int micro,u64 *out){
    if(!address){*out=~0ULL;return 0;}i64 *time=native_buffer(current_task,address,16,0);if(!time)return -14;
    u64 scale=micro?1000000:1000000000;
    if(time[0]<0||time[1]<0||(u64)time[1]>=scale)return -22;
    u64 seconds=time[0];if(absolute&&realtime)seconds=seconds>native_epoch_base?seconds-native_epoch_base:0;
    if(seconds>(~0ULL-timer_ticks-100)/100){*out=~0ULL;return 0;}
    *out=seconds*100+((u64)time[1]*100+scale-1)/scale+(absolute?0:timer_ticks);return 0;
}
static i64 native_poll_call(u64 n,u64 address,u64 count,u64 timeout,u64 mask,u64 masksize){
    NativeWait *w=&native_waits[current_task];int result=native_poll_scan(current_task,address,count,0);if(result<0)return result;
    if(!w->kind){*w=(NativeWait){.kind=3,.syscall=n,.address=address,.count=count};
        if(n==7)w->deadline=(int)timeout<0?~0ULL:timer_ticks+((u32)timeout+9ULL)/10;
        else{result=native_deadline(timeout,0,0,0,&w->deadline);if(result)return result;
            if(mask){u64 *value=masksize==8?native_buffer(current_task,mask,8,0):0;if(!value)return masksize==8?-14:-22;
                w->oldmask=native_process[current_task].sigmask;w->mask_changed=1;native_process[current_task].sigmask=*value&~((1ULL<<8)|(1ULL<<18));}}}
    if(result||timer_ticks>=w->deadline)return native_poll_scan(current_task,address,count,1);
    native_wait_blocks++;return -4096;
}
static i64 native_select_call(u64 count,u64 read,u64 write,u64 except,u64 timeout){
    NativeWait *w=&native_waits[current_task];
    if(!w->kind){*w=(NativeWait){.kind=4,.syscall=23,.count=count,.extra={read,write,except}};
        int error=native_deadline(timeout,0,0,1,&w->deadline);if(error)return error;}
    int result=native_select_scan(current_task,w,0);if(result<0)return result;
    if(result||timer_ticks>=w->deadline){if(timeout){u64 *out=native_buffer(current_task,timeout,16,1);if(!out)return -14;
            u64 left=w->deadline>timer_ticks?w->deadline-timer_ticks:0;out[0]=left/100;out[1]=(left%100)*10000;}
        return native_select_scan(current_task,w,1);}
    native_wait_blocks++;return -4096;
}
static i64 native_sleep_call(u64 n,u64 a,u64 b,u64 c,u64 d){
    NativeWait *w=&native_waits[current_task];
    if(n==230&&(a>1||(b&~1ULL)))return -22;
    if(!w->kind){*w=(NativeWait){.kind=5,.syscall=n,.address=n==35?b:d,.extra={n==230&&(b&1)}};int error=native_deadline(n==35?a:c,n==230&&(b&1),n==230&&a==0,0,&w->deadline);if(error)return error;
        if((n==35&&!a)||(n==230&&!c))return -14;}
    if(timer_ticks>=w->deadline)return 0;native_wait_blocks++;return -4096;
}
static i64 native_futex_call(u64 address,u64 operation,u64 value,u64 timeout,u64 other,u64 bits){
    if(address&3)return -22;u32 *word=native_buffer(current_task,address,4,0);if(!word)return -14;
    u32 op=operation&127;if(operation&~511ULL)return -38;
    u64 key=(operation&128)?((u64)native_space(current_task)<<32)|address:(native_alias_pt(current_task)[(address-USER_BASE)/4096]&0x000ffffffffff000ULL)|(address&4095);
    u64 mask=(op==9||op==10)?(u32)bits:0xffffffffU;if(!mask)return -22;
    if(op==3||op==4){
        if((int)value<0||(int)timeout<0||(other&3))return -22;
        if(!native_buffer(current_task,other,4,0))return -14;
        if(op==4&&*word!=(u32)bits)return -11;
        u64 destination=(operation&128)?((u64)native_space(current_task)<<32)|other:(native_alias_pt(current_task)[(other-USER_BASE)/4096]&0x000ffffffffff000ULL)|(other&4095);
        int woke=0,moved=0;
        for(int id=APP_FIRST;id<TASK_COUNT;id++){NativeWait *w=&native_waits[id];
            if(tasks[id].state!=WAIT_EVENT||w->kind!=6||w->key!=key)continue;
            if(woke<(int)value){w->awoken=1;tasks[id].state=RUNNABLE;woke++;native_wait_wakes++;}
            else if(moved<(int)timeout){w->key=destination;moved++;}}
        return woke+moved;
    }
    if(op==1||op==10){int count=0;if((int)value<0)return -22;
        for(int id=APP_FIRST;id<TASK_COUNT&&count<(int)value;id++){NativeWait *w=&native_waits[id];
            if(tasks[id].state==WAIT_EVENT&&w->kind==6&&w->key==key&&(w->bits&mask)){w->awoken=1;tasks[id].state=RUNNABLE;count++;native_wait_wakes++;}}
        return count;}
    if(op!=0&&op!=9)return -38;NativeWait *w=&native_waits[current_task];
    if(w->kind==6){if(w->awoken)return 0;if(timer_ticks>=w->deadline)return -110;}
    else{if(*word!=(u32)value)return -11;*w=(NativeWait){.kind=6,.syscall=202,.key=key,.bits=mask};
        int error=native_deadline(timeout,op==9,(operation&256)!=0,0,&w->deadline);if(error)return error;}
    native_wait_blocks++;return -4096;
}
static void native_wake_waiters(void){
    for(u32 id=APP_FIRST;id<TASK_COUNT;id++)if(tasks[id].state==WAIT_EVENT){NativeWait *w=&native_waits[id];int ready=timer_ticks>=w->deadline;
        u64 pending=native_signals(id)->pending&~native_process[id].sigmask;
        for(int sig=1;sig<=64;sig++)if(pending&(1ULL<<(sig-1))){NativeSigaction *action=&native_actions(id)[sig];
            if(action->handler==1||(!action->handler&&(sig==17||sig==18||sig==23||sig==28)))continue;
            w->interrupted=!(w->kind<=2&&(action->flags&0x10000000));ready=1;break;}
        if(w->kind==1)ready|=(native_readiness(id,(int)w->address)&(w->count|8|16|32))!=0;
        if(w->kind==2)for(int child=APP_FIRST;child<TASK_COUNT;child++)if(native_active[child]&&!native_process[child].thread&&native_process[child].parent==(int)native_process[id].tgid-100&&((tasks[child].state==DEAD&&!native_group_refs[child])||tasks[child].state==STOPPED))ready=1;
        if(w->kind==3)ready|=native_poll_scan(id,w->address,w->count,0)!=0;
        if(w->kind==4)ready|=native_select_scan(id,w,0)!=0;
        if(ready){tasks[id].state=RUNNABLE;native_wait_wakes++;}
    }
}
