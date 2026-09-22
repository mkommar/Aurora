/* Small bootstrap gate, not a substitute for the upstream GCC testsuite. */
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <pthread.h>
static _Atomic unsigned counter;
static void *worker(void *ignored) {
    (void)ignored;
    for(unsigned i=0;i<10000;i++)atomic_fetch_add(&counter,1);
    return NULL;
}
static long sum(int n,...) {
    va_list args;va_start(args,n);long s=0;
    for(int i=0;i<n;i++)s+=va_arg(args,long);
    va_end(args);return s;
}
static int compare(const void *a,const void *b) {
    int x=*(const int *)a,y=*(const int *)b;return (x>y)-(x<y);
}
int main(void) {
    uint32_t x=1;for(unsigned i=0;i<10000;i++)x=x*1664525U+1013904223U;
    assert(x==4089345937U);
    assert(sum(4,1L,-2L,3L,10000000000L)==10000000002L);
    volatile double a=1.5,b=2.25;assert(a*b==3.375 && b/a==1.5);
    int values[]={9,-5,2,0,2,100,-99};qsort(values,7,sizeof(int),compare);
    assert(values[0]==-99 && values[6]==100 && values[3]==2);
    char *buffer=malloc(8192);assert(buffer);memset(buffer,'x',8192);
    memmove(buffer+1,buffer,8191);assert(buffer[8191]=='x');free(buffer);
    jmp_buf escape;volatile int jumped=0;
    if(!setjmp(escape)){jumped=1;longjmp(escape,7);}assert(jumped==1);
    pthread_t threads[2];for(int i=0;i<2;i++)assert(!pthread_create(&threads[i],NULL,worker,NULL));
    for(int i=0;i<2;i++)assert(!pthread_join(threads[i],NULL));assert(counter==20000);
    puts("AURORA_COMPILER_CORPUS_PASS");return 0;
}
