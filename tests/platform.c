/* Platform regression: signal semantics, job control, demand paging, mremap,
 * interval timers, build-critical syscalls and batched storage. Compiled and
 * run inside Aurora by test-platform.py; no host compiler is involved. */
#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/statfs.h>
#include <sys/times.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sched.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <setjmp.h>
#include <time.h>
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s errno=%d\n",__LINE__,#x,errno); return 1; } } while(0)
static volatile sig_atomic_t outer_depth,nested_seen,alarm_count,usr2_count,segv_seen,chld_code,chld_status;
static volatile void *fault_address;
static sigjmp_buf recover;
static void nested_inner(int signal,siginfo_t *info,void *context){(void)context;if(signal==SIGUSR2&&outer_depth==1&&(info->si_code==SI_USER||info->si_code==SI_TKILL))nested_seen=1;usr2_count++;}
static void nested_outer(int signal,siginfo_t *info,void *context){
    (void)signal;(void)info;(void)context;outer_depth=1;raise(SIGUSR2); /* SIGUSR2 is not blocked in this handler: delivered on top of this frame */
    for(volatile int spin=0;spin<1000&&!nested_seen;spin++){}
    outer_depth=0;
}
static void segv_handler(int signal,siginfo_t *info,void *context){
    (void)context;segv_seen=signal;fault_address=info->si_addr;siglongjmp(recover,1);
}
static char alternate[16384];
static void alt_handler(int signal){char probe;segv_seen=(&probe>=alternate&&&probe<alternate+sizeof(alternate))?signal:-1;}
static void alarm_handler(int signal){(void)signal;alarm_count++;}
static void chld_handler(int signal,siginfo_t *info,void *context){(void)signal;(void)context;chld_code=info->si_code;chld_status=info->si_status;}
static void mask_probe(int signal){(void)signal;sigset_t now;sigprocmask(SIG_BLOCK,0,&now);nested_seen=sigismember(&now,SIGUSR2)?2:3;}
int main(int argc,char **argv){
    (void)argc;(void)argv;
    /* Nested delivery: a handler that raises an unblocked signal runs the inner handler before returning. */
    struct sigaction action={0};action.sa_sigaction=nested_outer;action.sa_flags=SA_SIGINFO|SA_NODEFER;sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGUSR1,&action,0)==0);
    action.sa_sigaction=nested_inner;CHECK(sigaction(SIGUSR2,&action,0)==0);
    CHECK(raise(SIGUSR1)==0);CHECK(nested_seen==1&&outer_depth==0&&usr2_count==1);
    puts("PASS nested signal delivery");
    /* Fault signals: SIGSEGV with si_addr, recovered through siglongjmp. */
    action.sa_sigaction=segv_handler;action.sa_flags=SA_SIGINFO;CHECK(sigaction(SIGSEGV,&action,0)==0);
    volatile char *bad=(volatile char *)0x40;
    if(sigsetjmp(recover,1)==0){*bad=1;CHECK(0);}
    CHECK(segv_seen==SIGSEGV&&fault_address==(void *)0x40);
    /* PROT_NONE on a lazily committed region faults with the mapped address. */
    char *lazy=mmap(0,1<<20,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(lazy!=MAP_FAILED);
    CHECK(mprotect(lazy+4096,4096,PROT_NONE)==0);segv_seen=0;
    if(sigsetjmp(recover,1)==0){lazy[4096]=1;CHECK(0);}
    CHECK(segv_seen==SIGSEGV&&fault_address==lazy+4096);
    CHECK(mprotect(lazy+4096,4096,PROT_READ|PROT_WRITE)==0);lazy[4096]=7;CHECK(lazy[4096]==7&&lazy[0]==0&&lazy[(1<<20)-1]==0);
    puts("PASS fault signals with si_addr");
    /* Alternate stack: the handler observes its frame inside the sigaltstack region. */
    stack_t alt={.ss_sp=alternate,.ss_size=sizeof(alternate),.ss_flags=0};CHECK(sigaltstack(&alt,0)==0);
    struct sigaction plain={0};plain.sa_handler=alt_handler;plain.sa_flags=SA_ONSTACK;CHECK(sigaction(SIGUSR1,&plain,0)==0);
    segv_seen=0;CHECK(raise(SIGUSR1)==0);CHECK(segv_seen==SIGUSR1);
    stack_t current;CHECK(sigaltstack(0,&current)==0&&current.ss_flags==0);
    puts("PASS sigaltstack");
    /* sigsuspend: the handler runs with the temporary mask; the original mask returns afterwards. */
    sigset_t block,old;sigemptyset(&block);sigaddset(&block,SIGUSR2);CHECK(sigprocmask(SIG_BLOCK,&block,&old)==0);
    plain.sa_handler=mask_probe;plain.sa_flags=0;CHECK(sigaction(SIGUSR1,&plain,0)==0);
    pid_t self=getpid();pid_t kicker=fork();CHECK(kicker>=0);
    if(kicker==0){usleep(100000);kill(self,SIGUSR1);_exit(0);}
    sigset_t wait_mask;sigfillset(&wait_mask);sigdelset(&wait_mask,SIGUSR1); /* SIGUSR2 blocked inside sigsuspend */
    nested_seen=0;CHECK(sigsuspend(&wait_mask)==-1&&errno==EINTR);CHECK(nested_seen==2);
    sigset_t after;CHECK(sigprocmask(SIG_BLOCK,0,&after)==0);CHECK(sigismember(&after,SIGUSR2)==1&&sigismember(&after,SIGTERM)==0);
    int status;CHECK(waitpid(kicker,&status,0)==kicker&&WIFEXITED(status));
    /* sigpending/sigtimedwait/sigqueue with a blocked, queued value. */
    union sigval value={.sival_int=4242};CHECK(sigqueue(self,SIGUSR2,value)==0);
    sigset_t pending;CHECK(sigpending(&pending)==0&&sigismember(&pending,SIGUSR2)==1);
    siginfo_t info;struct timespec brief={.tv_sec=1,.tv_nsec=0};
    CHECK(sigtimedwait(&block,&info,&brief)==SIGUSR2&&info.si_code==SI_QUEUE&&info.si_value.sival_int==4242&&info.si_pid==self);
    brief.tv_sec=0;brief.tv_nsec=50000000;CHECK(sigtimedwait(&block,&info,&brief)==-1&&errno==EAGAIN);
    CHECK(sigprocmask(SIG_SETMASK,&old,0)==0);
    puts("PASS sigsuspend, sigpending, sigtimedwait and sigqueue");
    /* Interval timers: SIGALRM repeats, getitimer reports the remaining time, pause returns on the signal. */
    plain.sa_handler=alarm_handler;plain.sa_flags=0;CHECK(sigaction(SIGALRM,&plain,0)==0);
    struct itimerval interval={.it_interval={0,20000},.it_value={0,20000}},remaining;
    CHECK(setitimer(ITIMER_REAL,&interval,0)==0);
    for(int i=0;i<3;i++)CHECK(pause()==-1&&errno==EINTR);
    CHECK(getitimer(ITIMER_REAL,&remaining)==0&&remaining.it_interval.tv_usec==20000);
    struct itimerval stop={0};CHECK(setitimer(ITIMER_REAL,&stop,&remaining)==0&&alarm_count>=3);
    alarm_count=0;CHECK(alarm(1)==0);CHECK(alarm(0)>=0&&alarm_count==0);
    puts("PASS interval timers and pause");
    /* Job control: the child stops on SIGSTOP, resumes on SIGCONT, and both transitions are reported. */
    action.sa_sigaction=chld_handler;action.sa_flags=SA_SIGINFO;CHECK(sigaction(SIGCHLD,&action,0)==0);
    pid_t child=fork();CHECK(child>=0);
    if(child==0){for(;;)pause();}
    usleep(50000);CHECK(kill(child,SIGSTOP)==0);
    CHECK(waitpid(child,&status,WUNTRACED)==child&&WIFSTOPPED(status)&&WSTOPSIG(status)==SIGSTOP);
    CHECK(chld_code==CLD_STOPPED);
    CHECK(kill(child,SIGCONT)==0);
    CHECK(waitpid(child,&status,WCONTINUED)==child&&WIFCONTINUED(status));
    CHECK(chld_code==CLD_CONTINUED);
    CHECK(kill(child,SIGTERM)==0);
    siginfo_t exit_info;memset(&exit_info,0,sizeof(exit_info));
    CHECK(waitid(P_PID,child,&exit_info,WEXITED)==0&&exit_info.si_pid==child&&exit_info.si_code==CLD_KILLED&&exit_info.si_status==SIGTERM);
    CHECK(chld_code==CLD_KILLED&&chld_status==SIGTERM);
    CHECK(waitpid(child,&status,WNOHANG)==-1&&errno==ECHILD);
    puts("PASS job control with WCONTINUED and waitid");
    /* Demand paging: a 256 MiB private mapping succeeds and only touched pages are committed. */
    size_t huge=256u<<20;char *big=mmap(0,huge,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(big!=MAP_FAILED);
    for(size_t offset=0;offset<huge;offset+=huge/64)big[offset]=(char)(offset>>20);
    for(size_t offset=0;offset<huge;offset+=huge/64)CHECK(big[offset]==(char)(offset>>20)&&big[offset+1]==0);
    pid_t heir=fork();CHECK(heir>=0);
    if(heir==0){for(size_t offset=0;offset<huge;offset+=huge/64)if(big[offset]!=(char)(offset>>20))_exit(1);big[0]=99;_exit(big[0]==99?0:2);}
    CHECK(waitpid(heir,&status,0)==heir&&WIFEXITED(status)&&WEXITSTATUS(status)==0&&big[0]==0);
    CHECK(munmap(big,huge)==0);
    /* mremap: grow in place, then relocate with MREMAP_MAYMOVE keeping contents. */
    char *region=mmap(0,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(region!=MAP_FAILED);
    memset(region,0x5a,8192);
    char *grown=mremap(region,8192,65536,MREMAP_MAYMOVE);CHECK(grown!=MAP_FAILED);
    CHECK(grown[0]==0x5a&&grown[8191]==0x5a&&grown[8192]==0&&grown[65535]==0);grown[65535]=3;
    char *blocker=mmap(grown+65536,4096,PROT_READ,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);CHECK(blocker==grown+65536);
    CHECK(mremap(grown,65536,131072,0)==MAP_FAILED&&errno==ENOMEM);
    char *moved=mremap(grown,65536,131072,MREMAP_MAYMOVE);CHECK(moved!=MAP_FAILED&&moved!=grown);
    CHECK(moved[0]==0x5a&&moved[8191]==0x5a&&moved[65535]==3&&moved[131071]==0);
    char *shrunk=mremap(moved,131072,4096,0);CHECK(shrunk==moved&&shrunk[0]==0x5a);
    CHECK(munmap(shrunk,4096)==0&&munmap(blocker,4096)==0);
    CHECK(madvise(lazy,4096,MADV_DONTNEED)==0&&msync(lazy,4096,MS_SYNC)==0&&mlock(lazy,4096)==0&&munlock(lazy,4096)==0);
    CHECK(munmap(lazy,1<<20)==0);
    puts("PASS demand paging and mremap");
    /* Build-critical syscalls used by configure scripts, make and the GNU tool chain. */
    int fd=open("/work/platform-a",O_CREAT|O_TRUNC|O_RDWR,0644);CHECK(fd>=0);CHECK(write(fd,"link me",7)==7);
    unlink("/work/platform-b");CHECK(link("/work/platform-a","/work/platform-b")==0);
    CHECK(link("/work/platform-a","/work/platform-b")==-1&&errno==EEXIST);
    struct stat st;CHECK(stat("/work/platform-b",&st)==0&&st.st_size==7);
    CHECK(truncate("/work/platform-a",3)==0&&stat("/work/platform-a",&st)==0&&st.st_size==3);
    CHECK(fallocate(fd,0,0,4096)==0&&fstat(fd,&st)==0&&st.st_size==4096);
    CHECK(flock(fd,LOCK_EX)==0&&flock(fd,LOCK_UN)==0&&flock(fd,99)==-1&&errno==EINVAL);
    struct statfs fs;CHECK(statfs("/work",&fs)==0&&fs.f_bsize>0&&fs.f_blocks>0&&fs.f_namelen==255);
    CHECK(fstatfs(fd,&fs)==0&&fs.f_bfree<=fs.f_blocks);
    close(fd);CHECK(unlink("/work/platform-b")==0&&unlink("/work/platform-a")==0);
    struct tms cpu;clock_t ticks=times(&cpu);CHECK(ticks!=(clock_t)-1);
    CHECK(prctl(PR_SET_NAME,"platform")==0);char name[16]={0};CHECK(prctl(PR_GET_NAME,name)==0&&strcmp(name,"platform")==0);
    struct rlimit limit;CHECK(getrlimit(RLIMIT_NOFILE,&limit)==0&&limit.rlim_cur>=64);CHECK(setrlimit(RLIMIT_CORE,&limit)==0);
    CHECK(getpriority(PRIO_PROCESS,0)==0&&setpriority(PRIO_PROCESS,0,5)==0);
    /* musl's sched_getscheduler/getparam/setscheduler wrappers return ENOSYS
       without entering the kernel, so the raw syscalls are exercised here. */
    CHECK(syscall(SYS_sched_getscheduler,0)==SCHED_OTHER&&sched_get_priority_max(SCHED_FIFO)==99&&sched_yield()==0);
    struct sched_param param={0};CHECK(syscall(SYS_sched_getparam,0,&param)==0&&syscall(SYS_sched_setscheduler,0,SCHED_OTHER,&param)==0);
    CHECK(syscall(SYS_membarrier,0,0)>0&&syscall(SYS_membarrier,1,0)==0);
    puts("PASS build-critical syscalls");
    /* Storage: a 2 MiB write and read-back drives multi-slot VirtIO submissions or IRQ-driven ATA. */
    size_t bulk=2u<<20;unsigned *pattern=malloc(bulk);CHECK(pattern);
    for(size_t i=0;i<bulk/4;i++)pattern[i]=(unsigned)i*2654435761u;
    fd=open("/work/platform-bulk",O_CREAT|O_TRUNC|O_RDWR,0644);CHECK(fd>=0);
    CHECK(write(fd,pattern,bulk)==(ssize_t)bulk&&fsync(fd)==0&&lseek(fd,0,SEEK_SET)==0);
    unsigned *readback=malloc(bulk);CHECK(readback);CHECK(read(fd,readback,bulk)==(ssize_t)bulk&&memcmp(pattern,readback,bulk)==0);
    close(fd);CHECK(unlink("/work/platform-bulk")==0);
    puts("PASS bulk storage round trip");
    return 0;
}
