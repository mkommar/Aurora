/* Optional development volume on ATA primary slave. Metadata lives above the
 * legacy 128 MiB configuration and is accessed only when the disk is present. */
typedef struct {char path[256];u64 sector,size;u32 capacity,kind;u8 pad[232];} NativeFile;
#define NFILES ((NativeFile *)0x08400000)
static u32 native_count,native_next;
static int native_ready;
static u8 native_sector[512];
static int ns_equal(const char *a,const char *b){while(*a&&*a==*b)a++,b++;return *a==*b;}
static u64 ns_length(const char *s){u64 n=0;while(s[n])n++;return n;}
static void ns_copy(char *d,const char *s){while((*d++=*s++)){} }
#include "partitions.h"
static int native_raw_disk(u32 sector,void *data,int write) {
    if(virtio_present)return virtio_transfer(sector,data,1,write?1:0);
    if(sector>=0x10000000)return 0;
    outb(0x1f6,0xf0|(sector>>24));for(int i=0;i<4;i++)(void)inb(0x3f6);
    if(!ata_wait(0)){serial("NATIVE disk select failed sector=");hex(sector);serial(" status=");hex(inb(0x1f7));serial("\r\n");return 0;}
    outb(0x1f2,1);outb(0x1f3,sector);outb(0x1f4,sector>>8);outb(0x1f5,sector>>16);
    outb(0x1f7,write?0x30:0x20);if(!ata_wait(1)){serial("NATIVE disk command failed sector=");hex(sector);serial(" status=");hex(inb(0x1f7));serial("\r\n");return 0;}
    u64 count=256;
    if(write)__asm__ volatile("rep outsw":"+S"(data),"+c"(count):"d"((u16)0x1f0):"memory");
    else __asm__ volatile("rep insw":"+D"(data),"+c"(count):"d"((u16)0x1f0):"memory");
    for(int i=0;i<4;i++)(void)inb(0x3f6);int ok=ata_wait(0);if(!ok){serial("NATIVE disk transfer failed\r\n");}return ok;
}
static int native_disk(u32 sector,void *data,int write){if(sector>=native_partition_sectors)return 0;return native_raw_disk(sector+native_partition_base,data,write);}
static int native_path(char *,const char *,const char *);
#include "fat_backend.h"
#include "ext2_backend.h"
static void native_fs_init(void){
    if(!native_partitions_init())return;
    fat_init();
    if(!native_disk(0,native_sector,0))return;
    for(int i=0;i<8;i++)if(native_sector[i]!=(u8)"AURDEV01"[i]){ext2_init();return;}
    native_count=*(u32 *)(native_sector+8);native_next=*(u32 *)(native_sector+12);
    if(native_count>4096||native_next<4097||native_next>=1048576)return;
    for(u32 i=0;i<native_count;i++) {
        if(!native_disk(i+1,&NFILES[i],0))return;NativeFile *f=&NFILES[i];
        if(f->path[255]||f->kind>2||f->size>(u64)f->capacity*512||
           (f->kind==1&&(f->sector<4097||f->sector>=native_next||f->capacity>native_next-f->sector)))return;
    }
    native_ready=1;serial("NATIVE: development volume mounted\r\n");
}
/* Lexical paths, with the toolchain's /usr -> / alias. */
static int native_path(char *out,const char *cwd,const char *input){
    char joined[512];u64 a=input[0]=='/'?0:ns_length(cwd),b=ns_length(input);
    if(a+b+2>sizeof(joined))return 0;
    memcpy(joined,cwd,a);if(a)joined[a++]='/';memcpy(joined+a,input,b+1);
    int length=1;out[0]='/';char *p=joined;
    while(*p){while(*p=='/')p++;if(!*p)break;char *begin=p;while(*p&&*p!='/')p++;int n=(int)(p-begin);
        if(n==1&&begin[0]=='.')continue;
        if(n==2&&begin[0]=='.'&&begin[1]=='.'){while(length>1&&out[length-1]!='/')length--;if(length>1)length--;continue;}
        if(!ext2_ready&&length==1&&n==3&&begin[0]=='u'&&begin[1]=='s'&&begin[2]=='r')continue;
        if(length+n+1>255)return 0;if(length>1)out[length++]='/';memcpy(out+length,begin,n);length+=n;
    }out[length]=0;return 1;
}
static int native_find(const char *path){
    if(!native_ready)return -1;char resolved[256];
    if(ext2_ready&&!(fat_ready&&fat_path(path))){int error=ext2_resolve(resolved,path,1);if(error)return error;path=resolved;}
    for(u32 i=0;i<native_count;i++)if(NFILES[i].kind&&ns_equal(NFILES[i].path,path))return (int)i;
    return fat_ready&&fat_path(path)?fat_find(path):ext2_ready?ext2_find(path):-1;
}
static int native_commit(int index){
    if(fat_ready&&fat_path(NFILES[index].path))return fat_commit(index);
    if(ext2_ready)return ext2_commit(index);
    if(!native_disk(index+1,&NFILES[index],1))return 0;
    memset(native_sector,0,512);memcpy(native_sector,"AURDEV01",8);
    *(u32 *)(native_sector+8)=native_count;*(u32 *)(native_sector+12)=native_next;
    /* POSIX writes update the device cache; fsync explicitly flushes it. */
    return native_disk(0,native_sector,1);
}
static i64 native_read(int index,u64 offset,void *buffer,u64 count){
    if(fat_ready&&fat_path(NFILES[index].path))return fat_io(index,offset,buffer,count,0);
    if(ext2_ready)return ext2_io(index,offset,buffer,count,0);
    NativeFile *f=&NFILES[index];if(f->kind==2)return -21;
    if(offset>=f->size)return 0;if(count>f->size-offset)count=f->size-offset;
    for(u64 done=0;done<count;){u64 pos=offset+done,n=512-(pos&511);if(n>count-done)n=count-done;
        if(n==512){if(!native_disk(f->sector+pos/512,(u8 *)buffer+done,0))return -5;}
        else {if(!native_disk(f->sector+pos/512,native_sector,0))return -5;memcpy((u8 *)buffer+done,native_sector+(pos&511),n);}done+=n;
    }return count;
}
static int native_writable(const char *p){return ext2_ready||(p[0]=='/'&&p[1]=='t'&&p[2]=='m'&&p[3]=='p'&&p[4]=='/')||(p[0]=='/'&&p[1]=='w'&&p[2]=='o'&&p[3]=='r'&&p[4]=='k'&&p[5]=='/');}
static int native_create(const char *path){
    if(fat_ready&&fat_path(path))return fat_create(path);
    if(ext2_ready)return ext2_create(path);
    if(!native_ready)return -5;if(!native_writable(path))return -30;
    u32 i;for(i=0;i<native_count;i++)if(!NFILES[i].kind)break;
    if(i==4096)return -28;
    u64 reused_sector=i<native_count?NFILES[i].sector:0;
    u32 reused_capacity=i<native_count?NFILES[i].capacity:0;
    if(!reused_capacity&&native_next+2048>1048576)return -28;
    if(i==native_count)native_count++;
    NativeFile *f=&NFILES[i];memset(f,0,sizeof(*f));ns_copy(f->path,path);f->kind=1;
    if(reused_capacity){f->sector=reused_sector;f->capacity=reused_capacity;}
    else {f->sector=native_next;f->capacity=2048;native_next+=2048;}
    if(!native_commit(i)){serial("NATIVE create commit failed\r\n");return -5;}return (int)i;
}
static i64 native_write(int index,u64 offset,const void *buffer,u64 count){
    if(fat_ready&&fat_path(NFILES[index].path))return fat_io(index,offset,(void *)buffer,count,1);
    if(ext2_ready)return ext2_io(index,offset,(void *)buffer,count,1);
    NativeFile *f=&NFILES[index];if(!native_writable(f->path))return -30;
    if(offset>32*1024*1024||count>32*1024*1024-offset)return -27;
    if(offset+count>(u64)f->capacity*512){
        u32 capacity=(u32)((offset+count+1048575)/1048576)*2048;
        if(native_next+capacity>1048576)return -28;u64 old=f->sector,new_sector=native_next;
        for(u64 n=0;n<(f->size+511)/512;n++)if(!native_disk(old+n,native_sector,0)||!native_disk(new_sector+n,native_sector,1))return -5;
        f->sector=new_sector;f->capacity=capacity;native_next+=capacity;
    }
    /* Zero holes; normal compiler output is sequential. */
    for(u64 pos=f->size;pos<offset;){u64 n=512-(pos&511);if(n>offset-pos)n=offset-pos;
        memset(native_sector,0,512);if(pos&511)if(!native_disk(f->sector+pos/512,native_sector,0))return -5;
        memset(native_sector+(pos&511),0,n);if(!native_disk(f->sector+pos/512,native_sector,1))return -5;pos+=n;
    }
    for(u64 done=0;done<count;){u64 pos=offset+done,n=512-(pos&511);if(n>count-done)n=count-done;
        if(n==512){if(!native_disk(f->sector+pos/512,(void *)((const u8 *)buffer+done),1))return -5;}
        else {memset(native_sector,0,512);if(pos<f->size||pos&511)if(!native_disk(f->sector+pos/512,native_sector,0))return -5;
            memcpy(native_sector+(pos&511),(const u8 *)buffer+done,n);if(!native_disk(f->sector+pos/512,native_sector,1))return -5;}done+=n;
    }
    if(offset+count>f->size)f->size=offset+count;
    return native_commit(index)?(i64)count:-5;
}
