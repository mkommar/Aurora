#define _GNU_SOURCE
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define CHECK(x) do { if(!(x)){printf("FAIL line %d: %s errno=%d\n",__LINE__,#x,errno);return 1;} } while(0)
int main(void){
    CHECK(mkdir("/work/vfs-test",0750)==0);
    int dir=open("/work/vfs-test",O_RDONLY|O_DIRECTORY);CHECK(dir>=0);
    int fd=openat(dir,"original",O_CREAT|O_RDWR,0640);CHECK(fd>=0);
    CHECK(write(fd,"abc",3)==3);CHECK(lseek(fd,8192,SEEK_SET)==8192);CHECK(write(fd,"z",1)==1);
    CHECK(ftruncate(fd,16384)==0);CHECK(lseek(fd,3,SEEK_SET)==3);char zeros[64];CHECK(read(fd,zeros,64)==64);
    for(int i=0;i<64;i++)CHECK(!zeros[i]);CHECK(fsync(fd)==0);close(fd);
    CHECK(rename("/work/vfs-test/original","/work/vfs-test/renamed")==0);
    CHECK(symlink("renamed","/work/vfs-test/link")==0);
    struct stat st;CHECK(lstat("/work/vfs-test/link",&st)==0 && S_ISLNK(st.st_mode));
    CHECK(stat("/work/vfs-test/link",&st)==0 && S_ISREG(st.st_mode) && st.st_size==16384);
    CHECK(chmod("/work/vfs-test/renamed",0600)==0);CHECK(stat("/work/vfs-test/renamed",&st)==0 && (st.st_mode&0777)==0600);
    DIR *stream=opendir("/work/vfs-test");CHECK(stream);int found=0;struct dirent *entry;
    while((entry=readdir(stream)))if(!strcmp(entry->d_name,"renamed"))found=1;closedir(stream);CHECK(found);
    CHECK(rmdir("/work/vfs-test")==-1 && errno==ENOTEMPTY);
    CHECK(unlink("/work/vfs-test/link")==0);CHECK(stat("/work/vfs-test/renamed",&st)==0);
    CHECK(unlinkat(dir,"renamed",0)==0);close(dir);CHECK(rmdir("/work/vfs-test")==0);
    puts("PASS ext2 directories, openat, rename, symlinks, modes and zero-filled growth");
    CHECK(mkdir("/exchange/transfer",0755)==0);
    fd=open("/exchange/transfer/long filename.txt",O_CREAT|O_TRUNC|O_RDWR,0666);CHECK(fd>=0);
    CHECK(write(fd,"Aurora exchange",15)==15);CHECK(fsync(fd)==0);CHECK(lseek(fd,0,SEEK_SET)==0);
    char text[16]={0};CHECK(read(fd,text,15)==15 && !strcmp(text,"Aurora exchange"));close(fd);
    stream=opendir("/exchange/transfer");CHECK(stream);found=0;
    while((entry=readdir(stream)))if(!strcmp(entry->d_name,"long filename.txt"))found=1;closedir(stream);CHECK(found);
    CHECK(unlink("/exchange/transfer/long filename.txt")==0);CHECK(rmdir("/exchange/transfer")==0);
    puts("PASS FAT32 long names, read/write, flush and directory listing");return 0;
}
