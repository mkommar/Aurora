#ifndef AURORA_USER_LIB_H
#define AURORA_USER_LIB_H
#include "../abi.h"
static inline i64 syscall(u64 n,u64 a,u64 b,u64 c) {
    __asm__ volatile("int $0x80" : "+a"(n) : "D"(a),"S"(b),"d"(c) : "memory","cc");
    return (i64)n;
}
static inline void yield(void) { syscall(SYS_YIELD,0,0,0); }
static inline i64 send(u64 to,const Message *m) { return syscall(SYS_SEND,to,(u64)m,0); }
static inline i64 receive(Message *m) { return syscall(SYS_RECV,(u64)m,0,0); }
static inline i64 poll(Message *m) { return syscall(SYS_POLL,(u64)m,0,0); }
static inline u64 ticks(void) { return syscall(SYS_TICKS,0,0,0); }
static inline u8 inb(u16 p) { return (u8)syscall(SYS_IN,p,0,0); }
static inline void outb(u16 p,u8 v) { syscall(SYS_OUT,p,v,1); }
static inline void outw(u16 p,u16 v) { syscall(SYS_OUT,p,v,2); }
static inline int len(const char *s) { int n=0;while(s[n])n++;return n; }
static inline int eq(const char *a,const char *b) { while(*a&&*a==*b)a++,b++;return *a==*b; }
static inline void serial(const char *s) { syscall(SYS_LOG,(u64)s,len(s),0); }
void *memset(void *p,int v,u64 n);
void *memcpy(void *d,const void *s,u64 n);
static inline i64 file_request(const char *name,void *buffer,u64 size,int write) {
    FileRequest r={0};int i=0;while(name[i]&&i<31){r.name[i]=name[i];i++;}
    if(name[i])return ERR_NAME;r.buffer=(u64)buffer;r.size=size;
    return syscall(write?SYS_FILE_WRITE:SYS_FILE_READ,(u64)&r,0,0);
}
#endif
