/* Filesystem semantics regression, compiled and run inside Aurora by
 * test-filesystems.py: hard links and inode identity, symlinks, ownership and
 * timestamps, rename/unlink with open descriptors and orphan parking, the
 * unified /aurorafs namespace, the FAT exchange volume and the read-only raw
 * devices used by fsck-aurora. Prints PASS lines; exits 0 when all hold. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#ifndef SYS_renameat2
#define SYS_renameat2 316
#endif
static int failures;
#define CHECK(cond) do{if(!(cond)){printf("FAIL line %d: %s (errno %d %s)\n",__LINE__,#cond,errno,strerror(errno));failures++;}}while(0)
static void pass(const char *label){printf("PASS %s\n",label);}
static int count_orphans(const char *dir){DIR *d=opendir(dir);if(!d)return -1;int n=0;struct dirent *e;while((e=readdir(d)))if(!strncmp(e->d_name,".aurora-orphan-",15))n++;closedir(d);return n;}
static int has_entry(const char *dir,const char *name){DIR *d=opendir(dir);if(!d)return 0;int found=0;struct dirent *e;while((e=readdir(d)))if(!strcmp(e->d_name,name))found=1;closedir(d);return found;}
static void write_file(const char *path,const char *text){int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0644);CHECK(fd>=0);if(fd>=0){CHECK(write(fd,text,strlen(text))==(ssize_t)strlen(text));close(fd);}}
static int read_file(const char *path,char *out,size_t cap){int fd=open(path,O_RDONLY);if(fd<0)return -1;ssize_t n=read(fd,out,cap-1);close(fd);if(n<0)return -1;out[n]=0;return (int)n;}
static void links(void){
    unlink("/work/fs-a");unlink("/work/fs-b");unlink("/work/fs-c");
    write_file("/work/fs-a","hello");
    CHECK(link("/work/fs-a","/work/fs-b")==0);
    struct stat a,b;CHECK(stat("/work/fs-a",&a)==0);CHECK(stat("/work/fs-b",&b)==0);
    CHECK(a.st_ino==b.st_ino);CHECK(a.st_nlink==2&&b.st_nlink==2);CHECK(a.st_dev==b.st_dev);
    int fd=open("/work/fs-b",O_WRONLY|O_APPEND);CHECK(fd>=0);CHECK(write(fd,"-world",6)==6);close(fd);
    char text[64];CHECK(read_file("/work/fs-a",text,sizeof(text))==11&&!strcmp(text,"hello-world"));
    CHECK(stat("/work/fs-a",&a)==0&&a.st_size==11);
    CHECK(link("/work/fs-a","/work/fs-b")<0&&errno==EEXIST);
    CHECK(link("/work","/work/fs-c")<0&&errno==EPERM);
    CHECK(unlink("/work/fs-a")==0);CHECK(stat("/work/fs-b",&b)==0&&b.st_nlink==1&&b.st_ino==a.st_ino);
    CHECK(read_file("/work/fs-b",text,sizeof(text))==11);
    CHECK(linkat(AT_FDCWD,"/work/fs-b",AT_FDCWD,"/work/fs-c",0)==0);CHECK(stat("/work/fs-c",&a)==0&&a.st_nlink==2);
    CHECK(unlink("/work/fs-c")==0);CHECK(unlink("/work/fs-b")==0);CHECK(stat("/work/fs-b",&b)<0&&errno==ENOENT);
    pass("hard links share one inode");
}
static void symlinks(void){
    unlink("/work/fs-target");unlink("/work/fs-link");unlink("/work/fs-dangling");
    write_file("/work/fs-target","target-data");
    CHECK(symlink("fs-target","/work/fs-link")==0);
    struct stat l,t;CHECK(lstat("/work/fs-link",&l)==0);CHECK(S_ISLNK(l.st_mode));CHECK(l.st_size==9);
    CHECK(stat("/work/fs-link",&t)==0);CHECK(S_ISREG(t.st_mode)&&t.st_size==11);CHECK(l.st_ino!=t.st_ino);
    char buffer[64];ssize_t n=readlink("/work/fs-link",buffer,sizeof(buffer));CHECK(n==9&&!memcmp(buffer,"fs-target",9));
    CHECK(read_file("/work/fs-link",buffer,sizeof(buffer))==11);
    CHECK(symlink("/work/does-not-exist","/work/fs-dangling")==0);
    CHECK(stat("/work/fs-dangling",&t)<0&&errno==ENOENT);CHECK(lstat("/work/fs-dangling",&l)==0);
    CHECK(symlink("x","/work/fs-link")<0&&errno==EEXIST);
    CHECK(unlink("/work/fs-link")==0);CHECK(stat("/work/fs-target",&t)==0);
    CHECK(unlink("/work/fs-dangling")==0);CHECK(unlink("/work/fs-target")==0);
    pass("symlinks resolve, lstat stops at the link");
}
static void ownership(void){
    unlink("/work/fs-owned");unlink("/work/fs-owned-link");
    write_file("/work/fs-owned","x");
    CHECK(chown("/work/fs-owned",1234,5678)==0);
    struct stat s;CHECK(stat("/work/fs-owned",&s)==0);CHECK(s.st_uid==1234&&s.st_gid==5678);
    CHECK(chown("/work/fs-owned",(uid_t)-1,42)==0);CHECK(stat("/work/fs-owned",&s)==0&&s.st_uid==1234&&s.st_gid==42);
    int fd=open("/work/fs-owned",O_RDONLY);CHECK(fd>=0);CHECK(fchown(fd,7,8)==0);close(fd);
    CHECK(stat("/work/fs-owned",&s)==0&&s.st_uid==7&&s.st_gid==8);
    CHECK(symlink("fs-owned","/work/fs-owned-link")==0);CHECK(lchown("/work/fs-owned-link",99,99)==0);
    struct stat l;CHECK(lstat("/work/fs-owned-link",&l)==0&&l.st_uid==99);CHECK(stat("/work/fs-owned",&s)==0&&s.st_uid==7);
    CHECK(fchownat(AT_FDCWD,"/work/fs-owned-link",55,66,AT_SYMLINK_NOFOLLOW)==0);CHECK(lstat("/work/fs-owned-link",&l)==0&&l.st_uid==55&&l.st_gid==66);
    CHECK(chmod("/work/fs-owned",0640)==0);CHECK(stat("/work/fs-owned",&s)==0&&(s.st_mode&07777)==0640);
    CHECK(fchmodat(AT_FDCWD,"/work/fs-owned",0755,0)==0);CHECK(stat("/work/fs-owned",&s)==0&&(s.st_mode&07777)==0755);
    struct timeval times[2]={{1600000000,0},{1500000000,0}};CHECK(utimes("/work/fs-owned",times)==0);
    CHECK(stat("/work/fs-owned",&s)==0&&s.st_atime==1600000000&&s.st_mtime==1500000000);
    struct timespec spec[2]={{0,UTIME_OMIT},{1400000000,0}};CHECK(utimensat(AT_FDCWD,"/work/fs-owned",spec,0)==0);
    CHECK(stat("/work/fs-owned",&s)==0&&s.st_atime==1600000000&&s.st_mtime==1400000000);
    CHECK(s.st_ctime>=1700000000); /* metadata changes bump ctime to the clock */
    CHECK(utimes("/work/fs-owned",0)==0);CHECK(stat("/work/fs-owned",&s)==0&&s.st_mtime>=1700000000);
    CHECK(unlink("/work/fs-owned-link")==0);CHECK(unlink("/work/fs-owned")==0);
    pass("ownership, modes and timestamps persist");
}
static void directories(void){
    rmdir("/work/fs-dir/sub");rmdir("/work/fs-dir");
    umask(027);CHECK(mkdir("/work/fs-dir",0777)==0);umask(022);
    struct stat s;CHECK(stat("/work/fs-dir",&s)==0);CHECK(S_ISDIR(s.st_mode)&&(s.st_mode&0777)==0750);CHECK(s.st_nlink==2);
    CHECK(mkdir("/work/fs-dir/sub",0755)==0);CHECK(stat("/work/fs-dir",&s)==0&&s.st_nlink==3);
    CHECK(rmdir("/work/fs-dir")<0&&errno==ENOTEMPTY);
    CHECK(rmdir("/work/fs-dir/sub")==0);CHECK(stat("/work/fs-dir",&s)==0&&s.st_nlink==2);
    CHECK(rmdir("/work/fs-dir")==0);
    pass("directory link counts follow subdirectories");
}
static void open_files(void){
    unlink("/work/fs-open");unlink("/work/fs-renamed");unlink("/work/fs-victim");
    int before=count_orphans("/");CHECK(before==0);
    write_file("/work/fs-open","open-data");int fd=open("/work/fs-open",O_RDWR);CHECK(fd>=0);
    CHECK(unlink("/work/fs-open")==0);CHECK(access("/work/fs-open",F_OK)<0&&errno==ENOENT);
    CHECK(count_orphans("/")==1); /* parked until the last close */
    char text[32];CHECK(pread(fd,text,9,0)==9&&!memcmp(text,"open-data",9));
    CHECK(pwrite(fd,"more",4,9)==4);struct stat s;CHECK(fstat(fd,&s)==0&&s.st_size==13);
    close(fd);CHECK(count_orphans("/")==0);
    write_file("/work/fs-open","a");write_file("/work/fs-victim","victim");
    int victim=open("/work/fs-victim",O_RDONLY);CHECK(victim>=0);
    CHECK(rename("/work/fs-open","/work/fs-victim")==0);
    CHECK(read_file("/work/fs-victim",text,sizeof(text))==1&&text[0]=='a');
    CHECK(read(victim,text,6)==6&&!memcmp(text,"victim",6)); /* replaced file stays readable */
    CHECK(count_orphans("/")==1);close(victim);CHECK(count_orphans("/")==0);
    CHECK(rename("/work/fs-victim","/work/fs-renamed")==0);CHECK(access("/work/fs-victim",F_OK)<0);
    CHECK(syscall(SYS_renameat2,AT_FDCWD,"/work/fs-renamed",AT_FDCWD,"/work/fs-renamed",1)==0); /* RENAME_NOREPLACE */
    write_file("/work/fs-victim","v");CHECK(syscall(SYS_renameat2,AT_FDCWD,"/work/fs-renamed",AT_FDCWD,"/work/fs-victim",1)<0&&errno==EEXIST);
    CHECK(unlink("/work/fs-victim")==0);CHECK(unlink("/work/fs-renamed")==0);
    pass("unlink and rename keep open descriptors, orphans vanish on close");
}
static void aurorafs(void){
    struct stat s;CHECK(stat("/aurorafs",&s)==0&&S_ISDIR(s.st_mode));
    unlink("/aurorafs/fs-native.txt");unlink("/aurorafs/fs-moved.txt");
    write_file("/aurorafs/fs-native.txt","written by a native process\n");
    CHECK(stat("/aurorafs/fs-native.txt",&s)==0);CHECK(S_ISREG(s.st_mode)&&s.st_size==28&&s.st_dev==3&&s.st_ino>=0x41000000);
    CHECK(has_entry("/aurorafs","fs-native.txt"));
    char text[64];CHECK(read_file("/aurorafs/fs-native.txt",text,sizeof(text))==28);
    int fd=open("/aurorafs/fs-native.txt",O_WRONLY);CHECK(fd>=0);CHECK(pwrite(fd,"WRITTEN",7,0)==7);close(fd);
    CHECK(read_file("/aurorafs/fs-native.txt",text,sizeof(text))==28&&!memcmp(text,"WRITTEN by",10));
    CHECK(truncate("/aurorafs/fs-native.txt",7)==0);CHECK(stat("/aurorafs/fs-native.txt",&s)==0&&s.st_size==7);
    CHECK(rename("/aurorafs/fs-native.txt","/aurorafs/fs-moved.txt")==0);CHECK(!has_entry("/aurorafs","fs-native.txt")&&has_entry("/aurorafs","fs-moved.txt"));
    CHECK(rename("/aurorafs/fs-moved.txt","/work/fs-moved.txt")<0&&errno==EXDEV);
    CHECK(mkdir("/aurorafs/dir",0755)<0&&errno==EPERM);
    CHECK(open("/aurorafs/bad name!",O_WRONLY|O_CREAT,0644)<0&&errno==EINVAL);
    CHECK(link("/aurorafs/fs-moved.txt","/work/x")<0&&errno==EXDEV);
    /* Legacy readers see the same file: the desktop's `load fs-moved.txt` path. */
    CHECK(has_entry("/aurorafs","readme.txt")); /* shipped with the boot disk */
    CHECK(read_file("/aurorafs/readme.txt",text,sizeof(text))>0);
    CHECK(unlink("/aurorafs/fs-moved.txt")==0);CHECK(stat("/aurorafs/fs-moved.txt",&s)<0&&errno==ENOENT);
    pass("AuroraFS is reachable as /aurorafs");
}
static void exchange(void){
    if(access("/exchange",F_OK)){printf("SKIP exchange volume absent\n");return;}
    unlink("/exchange/fs-fat.txt");
    write_file("/exchange/fs-fat.txt","fat-data");
    struct stat s;CHECK(stat("/exchange/fs-fat.txt",&s)==0);CHECK(s.st_size==8&&s.st_dev==2);CHECK(s.st_mtime>=1700000000); /* stamped by the RTC clock */
    int fd=open("/exchange/fs-fat.txt",O_RDONLY);CHECK(fd>=0);
    CHECK(unlink("/exchange/fs-fat.txt")==0);CHECK(count_orphans("/exchange")==1);
    char text[16];CHECK(read(fd,text,8)==8&&!memcmp(text,"fat-data",8));close(fd);CHECK(count_orphans("/exchange")==0);
    CHECK(rename("/work/nothing","/exchange/x")<0&&(errno==EXDEV||errno==ENOENT));
    pass("FAT exchange volume stamps time and parks open files");
}
static void raw_devices(void){
    int fd=open("/dev/disk",O_RDONLY);CHECK(fd>=0);
    struct stat s;CHECK(fstat(fd,&s)==0&&S_ISBLK(s.st_mode)&&s.st_size>0&&(s.st_size%512)==0);
    char sector[512];CHECK(pread(fd,sector,512,512)==512&&!memcmp(sector,"EFI PART",8));
    CHECK(pread(fd,sector,512,(off_t)s.st_size-512)==512&&!memcmp(sector,"EFI PART",8)); /* backup header */
    CHECK(lseek(fd,0,SEEK_END)==s.st_size);CHECK(pread(fd,sector,512,s.st_size)==0);
    CHECK(write(fd,sector,512)<0&&errno==EBADF);close(fd);
    CHECK(open("/dev/disk",O_RDWR)<0&&errno==EACCES);
    fd=open("/dev/boot",O_RDONLY);CHECK(fd>=0);CHECK(pread(fd,sector,512,512*512)==512&&!memcmp(sector,"AURFS01",8));close(fd);
    struct statfs f;CHECK(statfs("/work",&f)==0&&f.f_type==0xef53);CHECK(statfs("/exchange",&f)==0&&f.f_type==0x4d44);
    pass("raw devices are readable and read-only");
}
int main(void){
    links();symlinks();ownership();directories();open_files();aurorafs();exchange();raw_devices();
    printf(failures?"FILESYSTEM REGRESSION FAILED (%d)\n":"FILESYSTEM REGRESSION OK\n",failures);return failures?1:0;
}
