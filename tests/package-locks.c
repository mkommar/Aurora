#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if(!(x)){dprintf(2,"FAIL line %d: %s errno=%d\n",__LINE__,#x,errno);exit(1);} } while(0)
static const char *path="/work/package-lock",*alias="/work/package-lock-alias";
static int lock(int fd,int command,short type){struct flock f={.l_type=type,.l_whence=SEEK_SET};return fcntl(fd,command,&f);}
static void token(int fd){CHECK(write(fd,"x",1)==1);}
static void receive(int fd){char ch;CHECK(read(fd,&ch,1)==1);}
static void reap(pid_t pid){int status;CHECK(waitpid(pid,&status,0)==pid);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);}
static void alarm_handler(int signal){(void)signal;}
int main(void){
    unlink(path);unlink(alias);
    int fd=open(path,O_CREAT|O_RDWR,0600);CHECK(fd>=0);CHECK(link(path,alias)==0);
    CHECK(lock(fd,F_SETLK,F_WRLCK)==0);
    int ready[2],go[2];CHECK(pipe(ready)==0&&pipe(go)==0);
    pid_t parent=getpid(),pid=fork();CHECK(pid>=0);
    if(!pid){
        int other=open(alias,O_RDWR);CHECK(other>=0);
        struct flock query={.l_type=F_WRLCK,.l_whence=SEEK_SET};
        CHECK(fcntl(other,F_GETLK,&query)==0&&query.l_type==F_WRLCK&&query.l_pid==parent);
        CHECK(lock(other,F_SETLK,F_UNLCK)==0); /* fork does not inherit POSIX locks */
        CHECK(lock(other,F_SETLK,F_WRLCK)==-1&&errno==EAGAIN);
        token(ready[1]);receive(go[0]);
        CHECK(lock(other,F_SETLKW,F_WRLCK)==0);token(ready[1]);
        close(other);_exit(0);
    }
    receive(ready[0]);token(go[1]);usleep(100000);
    int aliasfd=open(alias,O_RDONLY);CHECK(aliasfd>=0);CHECK(close(aliasfd)==0);
    receive(ready[0]);reap(pid); /* closing any alias releases this process's lock */
    CHECK(lock(fd,F_SETLK,F_RDLCK)==0);
    pid=fork();CHECK(pid>=0);
    if(!pid){
        CHECK(lock(fd,F_SETLK,F_RDLCK)==0);
        CHECK(lock(fd,F_SETLK,F_WRLCK)==-1&&errno==EAGAIN);
        CHECK(flock(fd,LOCK_EX|LOCK_NB)==0); /* separate Linux lock families */
        CHECK(flock(fd,LOCK_UN)==0);_exit(0);
    }
    reap(pid);
    CHECK(lock(fd,F_SETLK,F_WRLCK)==0);CHECK(lock(fd,F_SETLK,F_UNLCK)==0);
    struct flock range={.l_type=F_WRLCK,.l_whence=SEEK_SET,.l_len=1};
    CHECK(fcntl(fd,F_SETLK,&range)==-1&&errno==EOPNOTSUPP);
    int ro=open(path,O_RDONLY);CHECK(ro>=0);CHECK(lock(ro,F_SETLK,F_WRLCK)==-1&&errno==EBADF);close(ro);
    CHECK(flock(fd,LOCK_EX)==0);
    int duplicate=dup(fd);CHECK(duplicate>=0);
    pid=fork();CHECK(pid>=0);
    if(!pid){
        int other=open(alias,O_RDWR);CHECK(other>=0);
        CHECK(flock(other,LOCK_EX|LOCK_NB)==-1&&errno==EAGAIN);
        token(ready[1]);receive(go[0]);
        CHECK(flock(other,LOCK_EX|LOCK_NB)==-1&&errno==EAGAIN);
        close(fd);CHECK(flock(other,LOCK_EX|LOCK_NB)==-1&&errno==EAGAIN);
        close(duplicate);CHECK(flock(other,LOCK_EX|LOCK_NB)==0);
        close(other);_exit(0);
    }
    receive(ready[0]);close(fd);close(duplicate);token(go[1]);reap(pid);
    fd=open(path,O_RDWR);CHECK(fd>=0);CHECK(lock(fd,F_SETLK,F_WRLCK)==0);
    pid=fork();CHECK(pid>=0);
    if(!pid){
        struct sigaction action={.sa_handler=alarm_handler};sigemptyset(&action.sa_mask);CHECK(sigaction(SIGALRM,&action,0)==0);
        alarm(1);CHECK(lock(fd,F_SETLKW,F_WRLCK)==-1&&errno==EINTR);_exit(0);
    }
    reap(pid);CHECK(lock(fd,F_SETLK,F_UNLCK)==0);
    pid=fork();CHECK(pid>=0);
    if(!pid){CHECK(lock(fd,F_SETLK,F_WRLCK)==0);token(ready[1]);pause();_exit(1);}
    receive(ready[0]);CHECK(kill(pid,SIGKILL)==0);int status;CHECK(waitpid(pid,&status,0)==pid&&WIFSIGNALED(status));
    CHECK(lock(fd,F_SETLK,F_WRLCK)==0);close(fd);
    unlink(alias);unlink(path);
    puts("PASS package locks: conflict, inode aliases, fork/dup ownership, close, blocking, signals and exit");return 0;
}
