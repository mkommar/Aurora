#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <unistd.h>
static atomic_int arrived,finished;
static atomic_ulong total;
static __thread unsigned tls;
static __thread unsigned gs_value;
static void *worker(void *arg){
    int cpu=(int)(long)arg;cpu_set_t mask;CPU_ZERO(&mask);CPU_SET(cpu,&mask);
    assert(!pthread_setaffinity_np(pthread_self(),sizeof mask,&mask));assert(sched_getcpu()==cpu);
    tls=cpu+100;atomic_fetch_add(&arrived,1);
    gs_value=cpu+200;assert(!syscall(SYS_arch_prctl,0x1001,&gs_value));
    while(atomic_load(&arrived)!=2)__asm__ volatile("pause");
    for(unsigned i=0;i<1000000;i++){atomic_fetch_add_explicit(&total,1,memory_order_relaxed);if(!(i%10000)){unsigned v;__asm__ volatile("mov %%gs:0,%0":"=r"(v));assert(v==(unsigned)cpu+200);assert(getpid()>0&&tls==(unsigned)cpu+100);}}
    __asm__ volatile("xor %%eax,%%eax; mov %%ax,%%gs":::"rax","memory");assert(getpid()>0);
    atomic_fetch_add(&finished,1);return 0;
}
int main(void){
    cpu_set_t mask;assert(!sched_getaffinity(0,sizeof mask,&mask));assert(CPU_COUNT(&mask)>=2);
    pthread_t a,b;assert(!pthread_create(&a,0,worker,(void *)0));assert(!pthread_create(&b,0,worker,(void *)1));
    assert(!pthread_join(a,0));assert(!pthread_join(b,0));assert(atomic_load(&total)==2000000&&atomic_load(&finished)==2);
    puts("PASS SMP pinned pthreads, shared atomics and per-CPU TLS");
    for(int round=0;round<12;round++){
        unsigned *p=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(p!=MAP_FAILED);*p=43;
        pid_t child=fork();assert(child>=0);if(!child){assert(*p==43);*p=91;_exit(0);}int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));assert(*p==43);
        assert(!mprotect(p,4096,PROT_READ));assert(*p==43);assert(!mprotect(p,4096,PROT_READ|PROT_WRITE));*p=52;assert(!munmap(p,4096));
    }
    puts("PASS SMP fork/COW, protection changes and page reclamation");return 0;
}
