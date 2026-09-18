#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s errno=%d\n",__LINE__,#x,errno); return 1; } } while(0)
static volatile sig_atomic_t handled;
static void handler(int signal){handled=signal;}
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
static int threads_ready,threads_go,thread_total,thread_fd;
static pid_t process_pid;
static __thread int thread_local_value;
static void *thread_worker(void *argument){
    long id=(long)argument;thread_local_value=100+id;
    if(getpid()!=process_pid||syscall(SYS_gettid)==process_pid)return (void *)99;
    pthread_mutex_lock(&mutex);threads_ready++;pthread_cond_broadcast(&condition);
    while(!threads_go)pthread_cond_wait(&condition,&mutex);pthread_mutex_unlock(&mutex);
    for(int i=0;i<1000;i++){pthread_mutex_lock(&mutex);thread_total++;pthread_mutex_unlock(&mutex);}
    if(id==1&&close(thread_fd))return (void *)98;
    return (void *)(thread_local_value==100+id?id:97);
}
static pthread_mutex_t robust;
static void *robust_worker(void *unused){(void)unused;return (void *)(long)pthread_mutex_lock(&robust);}
static void *shared_state_worker(void *unused){
    (void)unused;void *old=(void *)syscall(SYS_brk,0);if((void *)syscall(SYS_brk,(char *)old+4096)!=(char *)old+4096)return (void *)1;
    if(chdir("/"))return (void *)2;umask(027);return old;
}
static int last_worker_status;
static void *last_worker(void *argument){usleep(30000);write((int)(long)argument,"t",1);if(last_worker_status)_exit(last_worker_status);return 0;}
static void *terminal_last_worker(void *unused){(void)unused;usleep(50000);puts("THREAD_WORKER_FINISHED");_exit(7);}
struct SharedCondition {pthread_mutex_t mutex;pthread_cond_t condition;int ready;};
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"leader-exit")){
        pthread_t worker;CHECK(pthread_create(&worker,0,terminal_last_worker,0)==0);pthread_exit(0);
    }
    if(argc==3 && !strcmp(argv[1],"thread-lifetime")){
        pthread_t worker;CHECK(pthread_create(&worker,0,last_worker,(void *)(long)atoi(argv[2]))==0);pthread_exit(0);
    }
    int fd=open("/work/fd-test",O_CREAT|O_TRUNC|O_RDWR,0600);
    CHECK(fd>=0); CHECK(write(fd,"abcdef",6)==6); CHECK(lseek(fd,0,SEEK_SET)==0);
    int alias=dup(fd); char c=0; CHECK(alias>=0);
    CHECK(read(fd,&c,1)==1 && c=='a'); CHECK(read(alias,&c,1)==1 && c=='b');
    CHECK(fcntl(alias,F_SETFD,FD_CLOEXEC)==0); CHECK(fcntl(fd,F_GETFD)==0);
    CHECK(fcntl(alias,F_SETFL,O_APPEND)==0); CHECK(fcntl(fd,F_GETFL)&O_APPEND);
    CHECK(fcntl(alias,F_SETFL,0)==0);
    int high=fcntl(fd,F_DUPFD_CLOEXEC,20); CHECK(high>=20); CHECK(fcntl(high,F_GETFD)==FD_CLOEXEC); close(high);
    CHECK(dup3(fd,fd,0)==-1 && errno==EINVAL);
    pid_t child=fork(); CHECK(child>=0);
    if (!child) { if(read(alias,&c,1)!=1 || c!='c') _exit(9); _exit(0); }
    int status=0; CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    CHECK(read(fd,&c,1)==1 && c=='d'); close(alias); close(fd);
    puts("PASS shared offsets, status flags, descriptor flags and fork");
    int p[2]; CHECK(pipe2(p,O_NONBLOCK)==0);
    CHECK(read(p[0],&c,1)==-1 && errno==EAGAIN); close(p[0]); close(p[1]);
    CHECK(pipe(p)==0); child=fork(); CHECK(child>=0);
    if (!child) {
        close(p[0]); char buffer[8192]; memset(buffer,'x',sizeof(buffer)); size_t sent=0;
        while(sent<sizeof(buffer)) { ssize_t n=write(p[1],buffer+sent,sizeof(buffer)-sent); if(n<=0)_exit(8); sent+=n; }
        close(p[1]); _exit(0);
    }
    close(p[1]); size_t received=0; char buffer[113]; ssize_t n;
    while((n=read(p[0],buffer,sizeof(buffer)))>0) { for(int i=0;i<n;i++)CHECK(buffer[i]=='x'); received+=n; }
    CHECK(n==0 && received==8192); close(p[0]); CHECK(waitpid(child,&status,0)==child && WEXITSTATUS(status)==0);
    puts("PASS large pipe transfer, nonblocking read and EOF");
    for(int i=0;i<600;i++) { fd=open("/dev/null",O_WRONLY); CHECK(fd>=0); close(fd); }
    puts("PASS open-description reclamation");
    for(int i=0;i<100;i++) {
        char *memory=mmap(0,1024*1024,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        CHECK(memory!=MAP_FAILED && memory[0]==0 && memory[1024*1024-1]==0);
        memory[0]=42; memory[1024*1024-1]=43; CHECK(munmap(memory,1024*1024)==0);
    }
    puts("PASS mmap reuse and zeroing");
    char *guard=mmap(0,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    char *other=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(guard!=MAP_FAILED && other!=MAP_FAILED && guard!=other);
    unsigned char resident=0;CHECK(mincore(guard,4096,&resident)==0 && (resident&1));
    CHECK(mprotect(guard,4096,PROT_READ|PROT_WRITE)==0);guard[0]=42;
    CHECK(mmap(guard,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==guard && guard[0]==0);
    CHECK(munmap(guard,4096)==0);CHECK(mincore(guard,4096,&resident)==-1 && errno==ENOMEM);
    CHECK(munmap(other,4096)==0);
    CHECK(pipe(p)==0);CHECK(lseek(p[0],0,SEEK_CUR)==-1 && errno==ESPIPE);close(p[0]);close(p[1]);
    puts("PASS protected mapping accounting, residency, replacement and pipe seeking");
    CHECK(signal(SIGUSR1,handler)!=SIG_ERR); CHECK(raise(SIGUSR1)==0); CHECK(handled==SIGUSR1);
    sigset_t mask; sigemptyset(&mask); sigaddset(&mask,SIGUSR1); handled=0;
    CHECK(sigprocmask(SIG_BLOCK,&mask,0)==0); CHECK(raise(SIGUSR1)==0); CHECK(handled==0);
    CHECK(sigprocmask(SIG_UNBLOCK,&mask,0)==0); CHECK(handled==SIGUSR1);
    puts("PASS signal handler, return and blocked delivery");
    CHECK(pipe(p)==0);child=fork();CHECK(child>=0);
    if(!child){close(p[0]);usleep(30000);_exit(write(p[1],"r",1)!=1);}
    close(p[1]);struct pollfd watch={p[0],POLLIN,0};CHECK(poll(&watch,1,1000)==1 && (watch.revents&POLLIN));
    fd_set reads;FD_ZERO(&reads);FD_SET(p[0],&reads);struct timeval tv={1,0};
    CHECK(select(p[0]+1,&reads,0,0,&tv)==1 && FD_ISSET(p[0],&reads));CHECK(read(p[0],&c,1)==1 && c=='r');
    CHECK(waitpid(child,&status,0)==child && WEXITSTATUS(status)==0);watch.revents=0;CHECK(poll(&watch,1,0)==1 && (watch.revents&POLLHUP));close(p[0]);
    struct timespec before,after;CHECK(clock_gettime(CLOCK_MONOTONIC,&before)==0);CHECK(poll(0,0,30)==0);CHECK(clock_gettime(CLOCK_MONOTONIC,&after)==0);
    CHECK((after.tv_sec-before.tv_sec)*1000000000L+after.tv_nsec-before.tv_nsec>=20000000L);
    struct sigaction action={0};action.sa_handler=handler;CHECK(sigaction(SIGUSR1,&action,0)==0);handled=0;
    child=fork();CHECK(child>=0);if(!child){usleep(30000);kill(getppid(),SIGUSR1);_exit(0);}
    CHECK(poll(0,0,1000)==-1 && errno==EINTR && handled==SIGUSR1);CHECK(waitpid(child,&status,0)==child);
    puts("PASS poll/select readiness, hangup, timed sleep and signal interruption");
    int *shared=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);CHECK(shared!=MAP_FAILED);
    CHECK(syscall(SYS_futex,shared,0,7,0,0,0)==-1 && errno==EAGAIN);
    child=fork();CHECK(child>=0);if(!child){shared[1]=456;_exit(syscall(SYS_futex,shared,0,0,0,0,0)!=0);}
    int woke=0;for(int i=0;i<100&&!woke;i++){usleep(10000);woke=syscall(SYS_futex,shared,1,1,0,0,0);CHECK(woke>=0);}
    CHECK(woke==1);CHECK(waitpid(child,&status,0)==child && WEXITSTATUS(status)==0 && shared[1]==456);CHECK(munmap(shared,4096)==0);
    puts("PASS shared anonymous mappings and futex wait/wake");
    process_pid=getpid();thread_fd=open("/dev/null",O_WRONLY);CHECK(thread_fd>=0);thread_local_value=77;
    pthread_t threads[4];for(long i=0;i<4;i++)CHECK(pthread_create(&threads[i],0,thread_worker,(void *)(i+1))==0);
    pthread_mutex_lock(&mutex);while(threads_ready<4)pthread_cond_wait(&condition,&mutex);threads_go=1;pthread_cond_broadcast(&condition);pthread_mutex_unlock(&mutex);
    for(long i=0;i<4;i++){void *answer=0;CHECK(pthread_join(threads[i],&answer)==0 && answer==(void *)(i+1));}
    CHECK(thread_total==4000 && thread_local_value==77);CHECK(write(thread_fd,"x",1)==-1 && errno==EBADF);
    puts("PASS POSIX threads: shared VM/descriptors, TLS, mutex, condition and join");
    for(int i=0;i<32;i++){
        pthread_mutexattr_t attr;CHECK(pthread_mutexattr_init(&attr)==0);CHECK(pthread_mutexattr_setrobust(&attr,PTHREAD_MUTEX_ROBUST)==0);
        CHECK(pthread_mutex_init(&robust,&attr)==0);CHECK(pthread_mutexattr_destroy(&attr)==0);
        CHECK(pthread_create(&threads[0],0,robust_worker,0)==0);void *answer=(void *)99;
        CHECK(pthread_join(threads[0],&answer)==0 && answer==0);
        CHECK(pthread_mutex_lock(&robust)==EOWNERDEAD);CHECK(pthread_mutex_consistent(&robust)==0);
        CHECK(pthread_mutex_unlock(&robust)==0);CHECK(pthread_mutex_destroy(&robust)==0);
    }
    puts("PASS robust mutex owner death and 32 thread-slot reuse cycles");
    void *old_break=(void *)syscall(SYS_brk,0),*answer=0;CHECK(pthread_create(&threads[0],0,shared_state_worker,0)==0);
    CHECK(pthread_join(threads[0],&answer)==0 && answer==old_break);CHECK((void *)syscall(SYS_brk,0)==(char *)old_break+4096);
    char cwd[256];CHECK(getcwd(cwd,sizeof(cwd)) && !strcmp(cwd,"/"));CHECK(umask(022)==027);CHECK(chdir("/work")==0);
    CHECK(syscall(SYS_tgkill,getpid()+1,syscall(SYS_gettid),0)==-1 && errno==ESRCH);
    puts("PASS shared thread heap/cwd/umask and thread-group signal validation");
    struct SharedCondition *sync=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);CHECK(sync!=MAP_FAILED);
    pthread_mutexattr_t ma;pthread_condattr_t ca;CHECK(pthread_mutexattr_init(&ma)==0);CHECK(pthread_condattr_init(&ca)==0);
    CHECK(pthread_mutexattr_setpshared(&ma,PTHREAD_PROCESS_SHARED)==0);CHECK(pthread_condattr_setpshared(&ca,PTHREAD_PROCESS_SHARED)==0);
    CHECK(pthread_mutex_init(&sync->mutex,&ma)==0);CHECK(pthread_cond_init(&sync->condition,&ca)==0);sync->ready=0;
    child=fork();CHECK(child>=0);if(!child){pthread_mutex_lock(&sync->mutex);while(!sync->ready)pthread_cond_wait(&sync->condition,&sync->mutex);pthread_mutex_unlock(&sync->mutex);_exit(0);}
    usleep(30000);CHECK(pthread_mutex_lock(&sync->mutex)==0);sync->ready=1;CHECK(pthread_cond_signal(&sync->condition)==0);CHECK(pthread_mutex_unlock(&sync->mutex)==0);
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    CHECK(pthread_cond_destroy(&sync->condition)==0);CHECK(pthread_mutex_destroy(&sync->mutex)==0);CHECK(munmap(sync,4096)==0);
    CHECK(pthread_mutexattr_destroy(&ma)==0);CHECK(pthread_condattr_destroy(&ca)==0);puts("PASS process-shared mutex and condition across fork");
    for(int after_fork=0;after_fork<3;after_fork++){
        CHECK(pipe(p)==0);child=fork();CHECK(child>=0);
        if(!child){close(p[0]);
            if(after_fork){last_worker_status=after_fork==2?7:0;if(pthread_create(&threads[0],0,last_worker,(void *)(long)p[1]))_exit(1);pthread_exit(0);}
            char descriptor[16];snprintf(descriptor,sizeof(descriptor),"%d",p[1]);execl("/work/foundations","foundations","thread-lifetime",descriptor,(char *)0);_exit(1);}
        close(p[1]);CHECK(read(p[0],&c,1)==1 && c=='t');CHECK(read(p[0],&c,1)==0);close(p[0]);
        CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==(after_fork==2?7:0));
    }
    puts("PASS process-shared pthread synchronization and leader-exit lifetime");
    size_t large_size=192UL*1024*1024;char *large=mmap(0,large_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(large!=MAP_FAILED);large[0]=12;large[large_size-1]=34;
    CHECK(pipe(p)==0); pid_t children[8];
    for(int i=0;i<8;i++) { children[i]=fork(); CHECK(children[i]>=0); if(!children[i]) { close(p[1]);large[0]=55;large[large_size-1]=66;_exit(read(p[0],large+4096,1)!=1); } }
    close(p[0]); CHECK(write(p[1],"12345678",8)==8); close(p[1]);
    for(int i=0;i<8;i++)CHECK(waitpid(children[i],&status,0)==children[i] && WIFEXITED(status) && WEXITSTATUS(status)==0);
    CHECK(large[0]==12 && large[large_size-1]==34 && large[4096]==0);CHECK(munmap(large,large_size)==0);
    puts("PASS 192 MiB mapping and copy-on-write isolation for user and kernel writes");
    puts("PASS eight concurrent children with allocated pages");
    unlink("/work/fd-test"); return 0;
}
