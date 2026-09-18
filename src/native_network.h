#include <stdlib.h>
/* Linux x86-64 socket ABI. No user pointers survive a syscall or enter lwIP. */
static i64 native_network_wait(u64 n,u64 fd,u64 flags,i64 result){
    NativeFd *f=&native_process[current_task].fd[fd];
    if((result==-11||(n==42&&result==-115))&&!(flags&0x40)&&!(native_descriptions[f->description].flags&0x800)){
        NativeWait *w=&native_waits[current_task];
        if(n==42&&w->kind&&timer_ticks>=w->deadline)return -110;
        if(!w->kind)*w=(NativeWait){.kind=1,.syscall=n,.address=fd,.count=n==45||n==47?1:4,.deadline=n==42?timer_ticks+1500:~0ULL};
        native_wait_blocks++;return -4096;
    }
    return result;
}
static i64 native_network(u64 n,u64 a,u64 b,u64 c,u64 d,u64 e,u64 g){
    NativeProcess *p=&native_process[current_task];
    if(n==41){
        if(a!=2)return -97;if(b&~(0x80000ULL|0x800ULL|15ULL))return -22;
        int fd=native_fd_allocate();if(fd<0)return fd;int socket=network_socket(b&15,c);if(socket<0)return socket;
        int description=native_description(2|(b&0x800));if(!description){network_close(socket);return -23;}
        p->fd[fd]=(NativeFd){.kind=6,.index=socket,.description=description,.flags=b&0x80000};return fd;
    }
    if(n==53)return -97;
    if(a>=NATIVE_FDS||!p->fd[a].kind)return -9;if(p->fd[a].kind!=6)return -88;int socket=p->fd[a].index;
    if(n==43||n==50)return -95; /* Client transport only; no listening endpoint yet. */
    if(n==42||n==49){
        if(c<16||c>128)return -22;void *address=native_buffer(current_task,b,16,0);if(!address)return -14;
        i64 result=n==42?network_connect(socket,address,c):network_bind(socket,address,c);
        return n==42?native_network_wait(n,a,0,result):result;
    }
    if(n==48)return network_shutdown(socket,(int)b);
    if(n==51||n==52){
        u32 *size=native_buffer(current_task,c,4,1);if(!size)return -14;u32 length=*size;
        void *address=length?native_buffer(current_task,b,length<16?length:16,1):0;if(length&&!address)return -14;
        int result=network_name(socket,address,&length,n==52);if(!result)*size=length;return result;
    }
    if(n==54||n==55){
        u32 length;u32 *size=0;if(n==54){if(e>0xffffffffULL)return -22;length=e;}else{size=native_buffer(current_task,e,4,1);if(!size)return -14;length=*size;}
        void *value=length?native_buffer(current_task,d,length<4?length:4,n==55):0;
        if(length&&!value)return -14;int result=network_option(socket,b,c,value,&length,n==54);if(!result&&size)*size=length;return result;
    }
    if(n==44||n==45){
        if(c>0x100000)return -90;void *buffer=c?native_buffer(current_task,b,c,n==45):0;if(c&&!buffer)return -14;
        void *address=0;u32 length=0,*size=0;
        if(e){if(n==44){if(g<16||g>128)return -22;length=g;}else{size=native_buffer(current_task,g,4,1);if(!size)return -14;length=*size;}
            address=length?native_buffer(current_task,e,length<16?length:16,n==45):0;if(length&&!address)return -14;}
        i64 result=n==44?network_send(socket,buffer,c,d,address,length):network_recv(socket,buffer,c,d,address,size?&length:0);
        if(result>=0&&size)*size=length;return native_network_wait(n,a,d,result);
    }
    if(n==46||n==47){
        typedef struct {u64 name;u32 namelen,pad;u64 iov,count,control,controllen;u32 flags,pad2;} Header;
        Header *user=native_buffer(current_task,b,sizeof(Header),n==47);if(!user)return -14;Header h=*user;
        if(h.count>16)return -90;if(n==46&&h.controllen)return -95;
        u64 iov[32],total=0;if(h.count){void *v=native_buffer(current_task,h.iov,h.count*16,0);if(!v)return -14;memcpy(iov,v,h.count*16);}
        for(u64 i=0;i<h.count;i++){if(iov[2*i+1]>65536-total)return -90;total+=iov[2*i+1];
            if(iov[2*i+1]&&!native_buffer(current_task,iov[2*i],iov[2*i+1],n==47))return -14;}
        void *address=0;u32 length=h.namelen;if(h.name&&length){if(n==46&&(length<16||length>128))return -22;
            address=native_buffer(current_task,h.name,length<16?length:16,n==47);if(!address)return -14;}
        u8 *buffer=malloc(total?total:1);if(!buffer)return -12;
        if(n==46){u64 offset=0;for(u64 i=0;i<h.count;i++){if(iov[2*i+1])memcpy(buffer+offset,native_buffer(current_task,iov[2*i],iov[2*i+1],0),iov[2*i+1]);offset+=iov[2*i+1];}}
        i64 result=n==46?network_send(socket,buffer,total,c,address,length):network_recv(socket,buffer,total,c|0x20,address,&length);
        if(n==47&&result>=0){u64 amount=(u64)result>total?total:(u64)result,offset=0;
            for(u64 i=0;i<h.count&&offset<amount;i++){u64 count=iov[2*i+1];if(count>amount-offset)count=amount-offset;
                if(count)memcpy(native_buffer(current_task,iov[2*i],count,1),buffer+offset,count);offset+=count;}
            user->namelen=length;user->controllen=0;user->flags=(u64)result>total?0x20:0;if(!(c&0x20))result=amount;}
        free(buffer);return native_network_wait(n,a,c,result);
    }
    return -38;
}
