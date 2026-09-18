#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <unistd.h>
static atomic_int arrived,finished,mapping_done;
static atomic_ulong total;
static __thread unsigned tls;
static __thread unsigned gs_value;
static atomic_int protection_ready,protection_go;
static volatile unsigned *protection_page;
static void *protection_writer(void *arg){
    cpu_set_t mask;CPU_ZERO(&mask);CPU_SET((int)(long)arg,&mask);assert(!pthread_setaffinity_np(pthread_self(),sizeof mask,&mask));
    *protection_page=1;atomic_store(&protection_ready,1);
    while(!atomic_load(&protection_go))__asm__ volatile("pause");
    *protection_page=2;_exit(99); /* Must fault after the other CPU removes WRITE. */
}
static void *worker(void *arg){
    int cpu=(int)(long)arg;cpu_set_t mask;CPU_ZERO(&mask);CPU_SET(cpu,&mask);
    assert(!pthread_setaffinity_np(pthread_self(),sizeof mask,&mask));assert(sched_getcpu()==cpu);
    tls=cpu+100;atomic_fetch_add(&arrived,1);
    gs_value=cpu+200;assert(!syscall(SYS_arch_prctl,0x1001,&gs_value));
    while(atomic_load(&arrived)!=2)__asm__ volatile("pause");
    while(!atomic_load(&mapping_done))__asm__ volatile("pause");
    for(unsigned i=0;i<1000000;i++){atomic_fetch_add_explicit(&total,1,memory_order_relaxed);if(!(i%10000)){unsigned v;__asm__ volatile("mov %%gs:0,%0":"=r"(v));assert(v==(unsigned)cpu+200);assert(getpid()>0&&tls==(unsigned)cpu+100);}}
    __asm__ volatile("xor %%eax,%%eax; mov %%ax,%%gs":::"rax","memory");assert(getpid()>0);
    atomic_fetch_add(&finished,1);return 0;
}
int main(void){
    cpu_set_t mask;assert(!sched_getaffinity(0,sizeof mask,&mask));assert(CPU_COUNT(&mask)>=2);
    void *probe=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(probe!=MAP_FAILED);
    int last=0;for(int i=0;i<CPU_SETSIZE;i++)if(CPU_ISSET(i,&mask))last=i;
    pthread_t a,b;assert(!pthread_create(&a,0,worker,(void *)0));assert(!pthread_create(&b,0,worker,(void *)(long)last));
    while(atomic_load(&arrived)!=2)sched_yield();
    for(int i=0;i<32;i++){assert(!mprotect(probe,4096,PROT_READ));assert(!mprotect(probe,4096,PROT_READ|PROT_WRITE));}
    atomic_store(&mapping_done,1);
    assert(!pthread_join(a,0));assert(!pthread_join(b,0));assert(atomic_load(&total)==2000000&&atomic_load(&finished)==2);
    assert(!munmap(probe,4096));
    puts("PASS SMP pinned pthreads, shared atomics and per-CPU TLS");
    for(int round=0;round<12;round++){
        unsigned *p=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(p!=MAP_FAILED);*p=43;
        pid_t child=fork();assert(child>=0);if(!child){assert(*p==43);*p=91;_exit(0);}int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));assert(*p==43);
        assert(!mprotect(p,4096,PROT_READ));assert(*p==43);assert(!mprotect(p,4096,PROT_READ|PROT_WRITE));*p=52;assert(!munmap(p,4096));
    }
    puts("PASS SMP fork/COW, protection changes and page reclamation");
    pid_t victim=fork();assert(victim>=0);
    if(!victim){
        CPU_ZERO(&mask);CPU_SET(0,&mask);assert(!sched_setaffinity(0,sizeof mask,&mask));
        protection_page=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert((void *)protection_page!=MAP_FAILED);
        assert(!pthread_create(&a,0,protection_writer,(void *)(long)last));
        while(!atomic_load(&protection_ready))sched_yield();
        assert(!mprotect((void *)protection_page,4096,PROT_READ));atomic_store(&protection_go,1);
        pthread_join(a,0);_exit(98);
    }
    int status;assert(waitpid(victim,&status,0)==victim&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGSEGV);
    puts("PASS remote CPU loses stale write permission after TLB shootdown");return 0;
}
