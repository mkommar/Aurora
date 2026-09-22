#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
static void require(int ok, const char *name) {
    if (!ok) { perror(name); exit(1); }
    printf("PASS %s\n", name); fflush(stdout);
}
static void script(const char *path, const char *body, mode_t mode) {
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0700);
    require(fd>=0 && write(fd,body,strlen(body))==(ssize_t)strlen(body) && close(fd)==0 && chmod(path,mode)==0,"write script fixture");
}
static int run(const char *path, char **args, int expected_errno) {
    pid_t pid=fork(); require(pid>=0,"fork script child");
    if (!pid) { execve(path,args,environ); _exit(errno==expected_errno?0:99); }
    int status; require(waitpid(pid,&status,0)==pid,"wait script child");
    return WIFEXITED(status)?WEXITSTATUS(status):128;
}
int main(int argc,char **argv) {
    if (argc>1 && !strcmp(argv[1],"one two"))
        return argc==4 && !strcmp(argv[2],"/work/script-argument") && !strcmp(argv[3],"tail")?0:88;
    char *args[]={"ignored","tail",NULL};
    script("/work/script-basic","#!/bin/sh\ntest \"$1\" = tail && exit 23\nexit 77\n",0755);
    require(run("/work/script-basic",args,0)==23,"direct execve script arguments");
    script("/work/script-argument","#! /work/exec-scripts\tone two \t\n",0755);
    require(run("/work/script-argument",args,0)==0,"single optional interpreter argument");
    script("/work/script-missing","#!/work/no-such-interpreter\n",0755);
    require(run("/work/script-missing",args,ENOENT)==0,"missing interpreter ENOENT");
    script("/work/script-loop","#!/work/script-loop\n",0755);
    require(run("/work/script-loop",args,ELOOP)==0,"bounded interpreter recursion");
    script("/work/script-denied","#!/bin/sh\nexit 77\n",0644);
    require(run("/work/script-denied",args,EACCES)==0,"script executable permission");
    script("/work/script-empty","#! \t\n",0755);
    require(run("/work/script-empty",args,ENOEXEC)==0,"empty interpreter ENOEXEC");
    script("/work/script-plain","exit 77\n",0755);
    require(run("/work/script-plain",args,ENOEXEC)==0,"plain text remains ENOEXEC");
    char longline[300];memset(longline,'x',sizeof(longline));longline[0]='#';longline[1]='!';longline[298]='\n';longline[299]=0;
    script("/work/script-long",longline,0755);
    require(run("/work/script-long",args,ENOEXEC)==0,"truncated interpreter ENOEXEC");
    /* Cycle beyond the entire physical allocator pool, including the virtual
     * window reserved for kernel stacks. Concurrent resident memory stays low. */
    for(int round=0;round<5;round++) {
        size_t size=192UL*1024*1024;
        volatile unsigned char *memory=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        require(memory!=MAP_FAILED,"allocator cycle mmap");
        for(size_t i=0;i<size;i+=4096)memory[i]=(unsigned char)(i/4096+round);
        for(size_t i=0;i<size;i+=4096)if(memory[i]!=(unsigned char)(i/4096+round))exit(1);
        require(munmap((void *)memory,size)==0,"allocator cycle contents and release");
    }
    puts("AURORA_EXEC_SCRIPTS_PASS"); return 0;
}
