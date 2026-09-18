#include "include/aurora.h"
#include "../src/user/lib.h"
#include <stdarg.h>
size_t strlen(const char *s){size_t n=0;while(s[n])n++;return n;}
int strcmp(const char *a,const char *b){while(*a&&*a==*b)a++,b++;return (unsigned char)*a-(unsigned char)*b;}
int atoi(const char *s){unsigned n=0;int sign=1;while(*s==' '||*s=='\t')s++;if(*s=='-'){sign=-1;s++;}else if(*s=='+')s++;while(*s>='0'&&*s<='9')n=n*10+(unsigned)(*s++-'0');return sign<0?(int)(0U-n):(int)n;}
static int output(const char *s,size_t n){
    size_t offset=0;
    while(offset<n){Message m={0,MSG_CONSOLE,0,0,0};size_t count=n-offset;if(count>23)count=23;
        memcpy(&m.a,s+offset,count);i64 r;while((r=send(DESKTOP,&m))==ERR_FULL)yield();
        if(r<0)return -1;syscall(SYS_LOG,(u64)(s+offset),count,0);offset+=count;}
    return (int)n;
}
int putchar(int c){char b=(char)c;return output(&b,1)<0?-1:(unsigned char)c;}
int puts(const char *s){int n=output(s,strlen(s));if(n<0||putchar('\n')<0)return -1;return n+1;}
int printf(const char *format,...){
    va_list args;va_start(args,format);int total=0;
    while(*format){
        if(*format!='%'){if(putchar(*format++)<0)goto error;total++;continue;}
        format++;char buffer[32];const char *s=buffer;size_t n=0;
        if(*format=='s'){s=va_arg(args,const char *);if(!s)s="(null)";n=strlen(s);}
        else if(*format=='c'){buffer[0]=(char)va_arg(args,int);n=1;}
        else if(*format=='%'){buffer[0]='%';n=1;}
        else if(*format=='d'||*format=='u'||*format=='x'){
            unsigned value;int negative=0;if(*format=='d'){int v=va_arg(args,int);negative=v<0;value=negative?0U-(unsigned)v:(unsigned)v;}else value=va_arg(args,unsigned);
            unsigned base=*format=='x'?16:10;char reverse[16];int k=0;do{reverse[k++]="0123456789abcdef"[value%base];value/=base;}while(value);
            if(negative)buffer[n++]='-';while(k)buffer[n++]=reverse[--k];
        }else goto error;
        if(output(s,n)<0)goto error;total+=(int)n;format++;
    }
    va_end(args);return total;
error:va_end(args);return -1;
}
typedef struct Block {size_t size;struct Block *next;int available;size_t padding;} Block;
static union {unsigned long long alignment[2];unsigned char bytes[128*1024];} heap __attribute__((aligned(16)));
static Block *first;
void *malloc(size_t n){
    if(!n||n>sizeof(heap.bytes)-sizeof(Block))return NULL;n=(n+15)&~15ULL;
    if(!first){first=(Block *)heap.bytes;*first=(Block){sizeof(heap.bytes)-sizeof(Block),NULL,1,0};}
    for(Block *b=first;b;b=b->next)if(b->available&&b->size>=n){
        if(b->size>=n+sizeof(Block)+16){Block *tail=(Block *)((unsigned char *)(b+1)+n);*tail=(Block){b->size-n-sizeof(Block),b->next,1,0};b->next=tail;b->size=n;}
        b->available=0;return b+1;
    }
    return NULL;
}
void free(void *p){
    if(!p)return;Block *found=NULL;for(Block *b=first;b;b=b->next)if((void *)(b+1)==p){found=b;break;}
    if(!found)return;found->available=1;
    for(Block *b=first;b&&b->next;)if(b->available&&b->next->available){b->size+=sizeof(Block)+b->next->size;b->next=b->next->next;}else b=b->next;
}
long long aurora_readfile(const char *name,void *buffer,size_t capacity){return file_request(name,buffer,capacity,0);}
long long aurora_writefile(const char *name,const void *buffer,size_t size){return file_request(name,(void *)buffer,size,1);}
void exit(int status){syscall(SYS_EXIT,(u64)(i64)status,0,0);for(;;)yield();}
extern int main(int argc,char **argv);
void aurora_start(void){
    char arguments[128];char *argv[17];int argc=0;memcpy(arguments,(void *)0x5d1100,128);arguments[127]=0;
    char *p=arguments;while(*p&&argc<16){while(*p==' ')p++;if(!*p)break;argv[argc++]=p;while(*p&&*p!=' ')p++;if(*p)*p++=0;}
    argv[argc]=NULL;exit(main(argc,argv));
}
