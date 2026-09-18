#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <elf.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern int library_value(void);
static __thread int local=9;
static volatile sig_atomic_t handled;
static char alternate[16384];
static void handler(int sig){char marker;assert(sig==SIGUSR1);assert((uintptr_t)&marker>=(uintptr_t)alternate&&(uintptr_t)&marker<(uintptr_t)alternate+sizeof alternate);stack_t s;assert(!sigaltstack(0,&s)&&(s.ss_flags&SS_ONSTACK));handled++;}
static void *worker(void *unused){(void)unused;assert(local==9);local=51;assert(library_value()==40);assert(library_value()==41);return (void *)73;}
static void *fork_worker(void *unused){(void)unused;usleep(50000);return 0;}
static void bad_elf(void){
    int fd=open("./dyn",O_RDONLY);assert(fd>=0);struct stat st;assert(!fstat(fd,&st));char *image=malloc(st.st_size);assert(image&&read(fd,image,st.st_size)==st.st_size);close(fd);
    Elf64_Ehdr *h=(void *)image;Elf64_Phdr *ph=(void *)(image+h->e_phoff);int interp=-1;for(int i=0;i<h->e_phnum;i++)if(ph[i].p_type==PT_INTERP)interp=i;assert(interp>=0);
    strcpy(image+ph[interp].p_offset,"/missing/ld.so");ph[interp].p_filesz=15;
    fd=open("./badelf",O_CREAT|O_TRUNC|O_WRONLY,0700);assert(fd>=0&&write(fd,image,st.st_size)==st.st_size);close(fd);char *args[]={"./badelf",0};errno=0;assert(execv(args[0],args)==-1&&errno==ENOENT);
    h->e_phentsize=1;fd=open("./badelf",O_TRUNC|O_WRONLY);assert(fd>=0&&write(fd,image,st.st_size)==st.st_size);close(fd);errno=0;assert(execv(args[0],args)==-1&&errno==ENOEXEC);unlink("./badelf");free(image);
    puts("PASS malformed ELF and missing interpreter preserve running process");
}
int main(int argc,char **argv){
    if(argc>1){assert(argc==1801);for(int i=1;i<argc;i++)assert(!strcmp(argv[i],"long-link-argument"));return 0;}
    char **many=calloc(4098,sizeof(char *));assert(many);many[0]="./dyn";
    for(int i=1;i<=1800;i++)many[i]="long-link-argument";
    pid_t argument_child=fork();assert(argument_child>=0);if(!argument_child){execv(many[0],many);_exit(90);}
    int argument_status;assert(waitpid(argument_child,&argument_status,0)==argument_child&&WIFEXITED(argument_status)&&!WEXITSTATUS(argument_status));
    for(int i=1801;i<=4096;i++)many[i]="too-many";
    errno=0;assert(execv(many[0],many)==-1&&errno==E2BIG);free(many);
    puts("PASS large exec argument lists and bounded E2BIG rejection");
    int linkdir=open(".",O_RDONLY|O_DIRECTORY);assert(linkdir>=0);unlink("./dyn-link");
    assert(!symlinkat("dyn",linkdir,"dyn-link"));char linktarget[8];assert(readlink("./dyn-link",linktarget,sizeof linktarget)==3&&!memcmp(linktarget,"dyn",3));
    assert(!unlink("./dyn-link"));close(linkdir);
    assert(library_value()==40);local=10;pthread_t t;void *result;
    assert(!pthread_create(&t,0,worker,0));assert(!pthread_join(t,&result));assert(result==(void *)73&&local==10);assert(library_value()==41);
    void *lib=dlopen("./libprobe.so",RTLD_NOW|RTLD_LOCAL);assert(lib);int (*fn)(void)=dlsym(lib,"library_value");assert(fn&&fn()==42);assert(!dlclose(lib));
    lib=dlopen("./plugin.so",RTLD_NOW|RTLD_LOCAL);assert(lib);fn=dlsym(lib,"plugin_value");assert(fn&&fn()==77&&fn()==78);assert(!dlclose(lib));
    assert(!dlopen("./missing-library.so",RTLD_NOW));assert(dlerror());
    puts("PASS dynamic PIE, constructor, DSO TLS, pthread TLS and dlopen");
    gid_t gid;assert(getgroups(0,0)==1&&getgroups(1,&gid)==1&&gid==1000);
    struct timespec ts;assert(!clock_getres(CLOCK_MONOTONIC,&ts)&&ts.tv_nsec>0);errno=0;assert(clock_gettime(999,&ts)==-1&&errno==EINVAL);
    int ends[2];char pipebuf[4096]={0};assert(!pipe2(ends,O_NONBLOCK));assert(fcntl(ends[1],F_GETPIPE_SZ)==4096);assert(write(ends[1],pipebuf,sizeof pipebuf)==sizeof pipebuf);errno=0;assert(write(ends[1],pipebuf,1)==-1&&errno==EAGAIN);assert(read(ends[0],pipebuf,sizeof pipebuf)==sizeof pipebuf);close(ends[0]);close(ends[1]);
    int nullfd=open("/dev/null",O_WRONLY);assert(nullfd>=0&&lseek(nullfd,100,SEEK_SET)==0&&write(nullfd,"discard",7)==7);close(nullfd);
    int fd=open("posix-offset.txt",O_CREAT|O_TRUNC|O_RDWR,0600);assert(fd>=0);assert(write(fd,"abcdef",6)==6);assert(lseek(fd,2,SEEK_SET)==2);assert(pwrite(fd,"XY",2,3)==2);assert(lseek(fd,0,SEEK_CUR)==2);char b[7]={0};assert(pread(fd,b,6,0)==6&&b[3]=='X'&&b[4]=='Y');close(fd);unlink("posix-offset.txt");
    stack_t s={.ss_sp=alternate,.ss_size=sizeof alternate};assert(!sigaltstack(&s,0));struct sigaction sa={.sa_handler=handler,.sa_flags=SA_ONSTACK};sigemptyset(&sa.sa_mask);assert(!sigaction(SIGUSR1,&sa,0));assert(!raise(SIGUSR1));assert(handled==1);
    sigset_t blocked,old,empty,after;sigemptyset(&blocked);sigaddset(&blocked,SIGUSR1);sigemptyset(&empty);assert(!sigprocmask(SIG_BLOCK,&blocked,&old));
    pid_t sender=fork();assert(sender>=0);if(!sender){usleep(20000);kill(getppid(),SIGUSR1);_exit(0);}ts=(struct timespec){1,0};errno=0;assert(pselect(0,0,0,0,&ts,&empty)==-1&&errno==EINTR&&handled==2);assert(!sigprocmask(SIG_SETMASK,0,&after)&&sigismember(&after,SIGUSR1));assert(!sigprocmask(SIG_SETMASK,&old,0));assert(waitpid(sender,0,0)==sender);
    ts=(struct timespec){0,1000000};assert(!pselect(0,0,0,0,&ts,&empty));puts("PASS pselect timeout, atomic signal wait and mask restoration");
    assert(!sigaltstack(0,&s)&&!(s.ss_flags&SS_ONSTACK));s.ss_flags=SS_DISABLE;assert(!sigaltstack(&s,0));
    puts("PASS POSIX groups, clocks, positioned writes and alternate signal stack");bad_elf();
    pid_t child=fork();assert(child>=0);if(!child){assert(!pthread_create(&t,0,fork_worker,0));pthread_exit(0);}
    int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
    puts("PASS patched dynamic fork and last-thread exit");return 0;
}
