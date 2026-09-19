/* Interrupted-write workload for test-filesystems.py. It commits one file
 * with fsync, parks an open-but-unlinked file on ext2 and on the FAT exchange
 * volume (a crash orphan each), then streams metadata-heavy writes until the
 * harness cuts power (QMP quit). The harness reboots the same disk and checks
 * that the committed file is intact, the orphans were reclaimed and
 * fsck-aurora finds no structural damage. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static unsigned char block[4096];
static void fill(unsigned seed){for(unsigned i=0;i<sizeof(block);i++){seed=seed*1103515245u+12345u;block[i]=(unsigned char)(seed>>16);}}
static int write_all(int fd,const void *data,size_t size){const unsigned char *p=data;while(size){ssize_t n=write(fd,p,size);if(n<=0)return 0;p+=n;size-=n;}return 1;}
int main(void){
    setvbuf(stdout,0,_IONBF,0);
    int fd=open("/work/iw-committed.bin",O_WRONLY|O_CREAT|O_TRUNC,0644);if(fd<0){perror("committed");return 1;}
    for(unsigned i=0;i<256;i++){fill(i+1);if(!write_all(fd,block,sizeof(block))){perror("write");return 1;}}
    if(fsync(fd)){perror("fsync");return 1;}close(fd);
    printf("IW_COMMITTED\n");
    int orphan=open("/work/iw-orphan.bin",O_RDWR|O_CREAT|O_TRUNC,0644);if(orphan<0){perror("orphan");return 1;}
    fill(77);write_all(orphan,block,sizeof(block));if(unlink("/work/iw-orphan.bin")){perror("unlink");return 1;}
    printf("IW_ORPHAN\n");
    int fat=-1;if(!access("/exchange",F_OK)){fat=open("/exchange/iw-orphan.bin",O_RDWR|O_CREAT|O_TRUNC,0644);
        if(fat>=0){write_all(fat,block,sizeof(block));if(unlink("/exchange/iw-orphan.bin"))perror("fat unlink");else printf("IW_FAT_ORPHAN\n");}}
    fsync(orphan); /* the parked names are on disk now */
    mkdir("/work/iw-tree",0755);
    for(unsigned round=0;;round++){
        char name[64],renamed[64];snprintf(name,sizeof(name),"/work/iw-tree/stream-%u",round);snprintf(renamed,sizeof(renamed),"/work/iw-tree/renamed-%u",round);
        int f=open(name,O_WRONLY|O_CREAT|O_TRUNC,0644);if(f<0){perror(name);return 1;}
        for(unsigned i=0;i<64;i++){fill(round*64+i);if(!write_all(f,block,sizeof(block))){perror("stream");return 1;}}
        close(f);
        if(round%2==0)rename(name,renamed);
        if(round%3==2){snprintf(name,sizeof(name),"/work/iw-tree/renamed-%u",round-2);unlink(name);}
        write_all(orphan,block,sizeof(block));
        if(fat>=0)write_all(fat,block,sizeof(block));
        printf("IW_PROGRESS %u\n",round);
    }
}
