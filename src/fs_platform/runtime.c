/* Small kernel-side C support for the filesystem library. This heap is
 * reserved below the native page pool and used only with a development disk. */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef struct Block {size_t size;struct Block *next;int used;} Block;
static Block *heap;
void *malloc(size_t size){
    if(!size||size>0x1000000-sizeof(Block))return 0;size=(size+15)&~(size_t)15;
    if(!heap){heap=(Block *)0x0b000000;*heap=(Block){.size=0x1000000-sizeof(Block)};}
    for(Block *b=heap;b;b=b->next)if(!b->used&&b->size>=size){
        if(b->size>=size+sizeof(Block)+16){Block *n=(Block *)((char *)(b+1)+size);*n=(Block){.size=b->size-size-sizeof(Block),.next=b->next};b->next=n;b->size=size;}
        b->used=1;return b+1;
    }
    return 0;
}
void free(void *p){if(!p)return;((Block *)p-1)->used=0;for(Block *b=heap;b&&b->next;)if(!b->used&&!b->next->used){b->size+=sizeof(Block)+b->next->size;b->next=b->next->next;}else b=b->next;}
void *calloc(size_t n,size_t size){if(size&&n>SIZE_MAX/size)return 0;void *p=malloc(n*size);if(p)memset(p,0,n*size);return p;}
void *realloc(void *p,size_t size){if(!p)return malloc(size);if(!size){free(p);return 0;}Block *b=(Block *)p-1;if(size<=b->size)return p;void *n=malloc(size);if(n){memcpy(n,p,b->size);free(p);}return n;}
void *memmove(void *d,const void *s,size_t n){unsigned char *a=d;const unsigned char *b=s;if(a<b)while(n--)*a++=*b++;else while(n){n--;a[n]=b[n];}return d;}
int memcmp(const void *a,const void *b,size_t n){const unsigned char *x=a,*y=b;while(n--){if(*x!=*y)return *x-*y;x++;y++;}return 0;}
size_t strlen(const char *s){size_t n=0;while(s[n])n++;return n;}
int strcmp(const char *a,const char *b){while(*a&&*a==*b)a++,b++;return (unsigned char)*a-(unsigned char)*b;}
int strncmp(const char *a,const char *b,size_t n){while(n--){if(*a!=*b||!*a)return (unsigned char)*a-(unsigned char)*b;a++;b++;}return 0;}
char *strcpy(char *d,const char *s){char *r=d;while((*d++=*s++)){}return r;}
char *strchr(const char *s,int c){do{if(*s==(char)c)return (char *)s;}while(*s++);return 0;}
char *strncpy(char *d,const char *s,size_t n){size_t i=0;for(;i<n&&s[i];i++)d[i]=s[i];for(;i<n;i++)d[i]=0;return d;}
void qsort(void *base,size_t count,size_t size,int (*compare)(const void *,const void *)){
    char *p=base;for(size_t i=1;i<count;i++)for(size_t j=i;j&&compare(p+(j-1)*size,p+j*size)>0;j--)for(size_t k=0;k<size;k++){char c=p[(j-1)*size+k];p[(j-1)*size+k]=p[j*size+k];p[j*size+k]=c;}
}
