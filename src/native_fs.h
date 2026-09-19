/* Optional development volume on ATA primary slave. Metadata lives above the
 * legacy 128 MiB configuration and is accessed only when the disk is present. */
typedef struct {char path[256];u64 sector,size;u32 capacity,kind;u8 pad[232];} NativeFile;
#define NATIVE_FILE_CACHE 16384
#define NFILES ((NativeFile *)0x0d100000)
static u32 native_count,native_next;
static int native_ready;
static u8 native_sector[512];
static int ns_equal(const char *a,const char *b){while(*a&&*a==*b)a++,b++;return *a==*b;}
static u64 ns_length(const char *s){u64 n=0;while(s[n])n++;return n;}
static void ns_copy(char *d,const char *s){while((*d++=*s++)){} }
#include "partitions.h"
static int native_raw_disk(u32 sector,void *data,int write) {
    if(virtio_present)return virtio_transfer(sector,data,1,write?1:0);
    int ok=ata_transfer(sector,data,write,1);
    if(!ok){serial("NATIVE disk transfer failed sector=");hex(sector);serial(" status=");hex(inb(0x1f7));serial("\r\n");}return ok;
}
/* Multi-sector ranges become one batched VirtIO submission per 512 KiB. */
static int native_raw_disk_range(u32 sector,void *data,u32 count,int write){
    for(u32 done=0;done<count;){u32 n=count-done;
        if(virtio_present){u32 limit=virtio_slots*VIRTIO_SLOT_SECTORS;if(n>limit)n=limit;if(!virtio_transfer(sector+done,(u8 *)data+(u64)done*512,n,write?1:0))return 0;}
        else{n=1;if(!native_raw_disk(sector+done,(u8 *)data+(u64)done*512,write))return 0;}
        done+=n;}
    return 1;
}
static int native_disk(u32 sector,void *data,int write){if(sector>=native_partition_sectors)return 0;return native_raw_disk(sector+native_partition_base,data,write);}
/* Read-only raw devices for checkers: /dev/disk is the development disk,
 * /dev/boot the AuroraFS boot disk. Whole sectors go straight to the caller. */
static u64 native_device_size(int device){return device?32768ULL*512:native_disk_sectors*512;}
static i64 native_device_read(int device,u64 offset,void *buffer,u64 count){
    u64 size=native_device_size(device);if(offset>=size)return 0;if(count>size-offset)count=size-offset;
    for(u64 done=0;done<count;){u64 pos=offset+done,n=512-(pos&511);if(n>count-done)n=count-done;u32 lba=(u32)(pos/512);
        if(n==512&&!device){u64 whole=(count-done)/512;if(whole>1024)whole=1024;if(!native_raw_disk_range(lba,(u8 *)buffer+done,(u32)whole,0))return -5;done+=whole*512;continue;}
        if(!(device?sector_io(lba,native_sector,0):native_raw_disk(lba,native_sector,0)))return -5;
        memcpy((u8 *)buffer+done,native_sector+(pos&511),n);done+=n;}
    return count;
}
static int native_path(char *,const char *,const char *);
#include "fat_backend.h"
#include "ext2_backend.h"
/* AuroraFS appears to native processes as /aurorafs: up to 32 flat files of
 * 64 KiB in fixed extents, so writes stay in place and never move data. The
 * cached entry remembers its slot and is honoured only while the name matches. */
static int aurorafs_path(const char *p){const char *prefix="/aurorafs";for(int i=0;i<9;i++)if(p[i]!=prefix[i])return 0;return fs_ready&&(!p[9]||p[9]=='/');}
static const char *aurorafs_name(const char *p){return p[9]?p+10:"";}
static int aurorafs_cache(const char *path,int kind,u64 size,u32 slot){
    u32 index;for(index=0;index<native_count;index++)if(!NFILES[index].kind)break;
    if(index==NATIVE_FILE_CACHE)return -28;if(index==native_count)native_count++;
    NativeFile *f=&NFILES[index];memset(f,0,sizeof(*f));ns_copy(f->path,path);f->kind=kind;f->size=size;f->sector=slot;
    *(u32 *)f->pad=kind==2?0755:0644;*(u32 *)(f->pad+4)=1;return (int)index;
}
static int aurorafs_find(const char *path){
    const char *name=aurorafs_name(path);if(!*name)return aurorafs_cache(path,2,0,0);
    if(!name_valid(name))return -2;int slot=file_find(name);if(slot<0)return -2;
    return aurorafs_cache(path,1,directory[slot].size,slot);
}
static int aurorafs_slot(NativeFile *f){
    if(f->kind==2)return -1;u32 slot=(u32)f->sector;const char *name=aurorafs_name(f->path);
    if(slot<FS_FILES&&directory[slot].used&&name_equal(directory[slot].name,name))return (int)slot;
    int found=file_find(name);if(found>=0)f->sector=found;return found; /* legacy calls may have moved the name */
}
static int aurorafs_create(const char *path){
    const char *name=aurorafs_name(path);if(!name_valid(name))return -22;if(file_find(name)>=0)return -17;
    int slot=file_free_slot();if(slot<0)return -28;
    memset(&directory[slot],0,sizeof(FileEntry));memcpy(directory[slot].name,name,ns_length(name));directory[slot].used=1;
    if(!file_write_directory(slot)){directory[slot].used=0;return -5;}return aurorafs_find(path);
}
/* Byte span inside a slot's extent; NULL data writes zeros. */
static int aurorafs_span(int slot,u64 pos,u8 *data,u64 n,int write){
    for(u64 done=0;done<n;){u64 at=pos+done,m=512-(at&511);if(m>n-done)m=n-done;u32 lba=FS_DATA_LBA+slot*128+(u32)(at/512);
        if(m==512&&data){if(!sector_io(lba,data+done,write))return 0;}
        else{if(!sector_io(lba,disk_sector,0))return 0;
            if(write){if(data)memcpy(disk_sector+(at&511),data+done,m);else memset(disk_sector+(at&511),0,m);if(!sector_io(lba,disk_sector,1))return 0;}
            else memcpy(data+done,disk_sector+(at&511),m);}
        done+=m;}
    return 1;
}
static i64 aurorafs_io(int index,u64 offset,void *buffer,u64 count,int write){
    NativeFile *f=&NFILES[index];if(f->kind==2)return -21;int slot=aurorafs_slot(f);if(slot<0)return -2;u64 size=directory[slot].size;
    if(!write){if(offset>=size)return 0;if(count>size-offset)count=size-offset;return aurorafs_span(slot,offset,buffer,count,0)?(i64)count:-5;}
    if(offset>FS_MAX_SIZE||count>FS_MAX_SIZE-offset)return -27;
    if(offset>size&&!aurorafs_span(slot,size,0,offset-size,1))return -5;
    if(!aurorafs_span(slot,offset,buffer,count,1))return -5;
    if(offset+count>size){if(!disk_flush())return -5;directory[slot].size=(u32)(offset+count);if(!file_write_directory(slot))return -5;}
    f->size=directory[slot].size;return count;
}
static int aurorafs_commit(int index){
    NativeFile *f=&NFILES[index];if(f->kind==2)return 1;int slot=aurorafs_slot(f);if(slot<0)return !f->kind;
    if(!f->kind){memset(&directory[slot],0,sizeof(FileEntry));return file_write_directory(slot);}
    if(f->size>FS_MAX_SIZE)return 0;u64 size=directory[slot].size;
    if(f->size>size&&!aurorafs_span(slot,size,0,f->size-size,1))return 0;
    if(f->size!=size){directory[slot].size=(u32)f->size;if(!disk_flush()||!file_write_directory(slot))return 0;}
    return 1;
}
static int aurorafs_rename(const char *from,const char *to){
    const char *name=aurorafs_name(to);if(!name_valid(name))return -22;int slot=file_find(aurorafs_name(from));if(slot<0)return -2;
    int old=file_find(name);if(old>=0&&old!=slot){memset(&directory[old],0,sizeof(FileEntry));if(!file_write_directory(old))return -5;}
    memset(directory[slot].name,0,32);memcpy(directory[slot].name,name,ns_length(name));return file_write_directory(slot)?0:-5;
}
static void native_fs_init(void){
    native_disk_sectors=virtio_present?virtio_sectors:ata_identify(1);
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
/* Paths served by a backend other than the ext2 volume skip symlink resolution. */
static int native_foreign(const char *path){return (fat_ready&&fat_path(path))||aurorafs_path(path);}
static int native_find(const char *path){
    if(!native_ready&&!aurorafs_path(path))return -1;char resolved[256];
    if(ext2_ready&&!native_foreign(path)){int error=ext2_resolve(resolved,path,1);if(error)return error;path=resolved;}
    for(u32 i=0;i<native_count;i++)if(NFILES[i].kind&&ns_equal(NFILES[i].path,path))return (int)i;
    return aurorafs_path(path)?aurorafs_find(path):fat_ready&&fat_path(path)?fat_find(path):ext2_ready?ext2_find(path):-1;
}
static int native_commit(int index){
    if(aurorafs_path(NFILES[index].path))return aurorafs_commit(index);
    if(fat_ready&&fat_path(NFILES[index].path))return fat_commit(index);
    if(ext2_ready)return ext2_commit(index);
    if(!native_disk(index+1,&NFILES[index],1))return 0;
    memset(native_sector,0,512);memcpy(native_sector,"AURDEV01",8);
    *(u32 *)(native_sector+8)=native_count;*(u32 *)(native_sector+12)=native_next;
    /* POSIX writes update the device cache; fsync explicitly flushes it. */
    return native_disk(0,native_sector,1);
}
static i64 native_read(int index,u64 offset,void *buffer,u64 count){
    if(aurorafs_path(NFILES[index].path))return aurorafs_io(index,offset,buffer,count,0);
    if(fat_ready&&fat_path(NFILES[index].path))return fat_io(index,offset,buffer,count,0);
    if(ext2_ready)return ext2_io(index,offset,buffer,count,0);
    NativeFile *f=&NFILES[index];if(f->kind==2)return -21;
    if(offset>=f->size)return 0;if(count>f->size-offset)count=f->size-offset;
    for(u64 done=0;done<count;){u64 pos=offset+done,n=512-(pos&511);if(n>count-done)n=count-done;
        if(n==512){if(!native_disk(f->sector+pos/512,(u8 *)buffer+done,0))return -5;}
        else {if(!native_disk(f->sector+pos/512,native_sector,0))return -5;memcpy((u8 *)buffer+done,native_sector+(pos&511),n);}done+=n;
    }return count;
}
static int native_writable(const char *p){return ext2_ready||aurorafs_path(p)||(p[0]=='/'&&p[1]=='t'&&p[2]=='m'&&p[3]=='p'&&p[4]=='/')||(p[0]=='/'&&p[1]=='w'&&p[2]=='o'&&p[3]=='r'&&p[4]=='k'&&p[5]=='/');}
static int native_create(const char *path){
    if(aurorafs_path(path))return aurorafs_create(path);
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
    if(aurorafs_path(NFILES[index].path))return aurorafs_io(index,offset,(void *)buffer,count,1);
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
