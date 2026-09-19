/* Pinned lwext4, configured for ext2, behind Aurora's development VFS. */
#include <ext4.h>
#include <ext4_blockdev.h>
#include <ext4_super.h>
static int ext2_ready;
static int ext2_resolve(char *out,const char *input,int follow_last){
    ns_copy(out,input);
    for(int links=0;links<40;links++){
        char prefix[256];int changed=0;
        for(u64 end=1;;end++)if(out[end]=='/'||!out[end]){
            if(!out[end]&&!follow_last)return 0;
            memcpy(prefix,out,end);prefix[end]=0;uint32_t mode;
            int error=ext4_mode_get(prefix,&mode);if(error)return -error;
            if((mode&0170000)==0120000){
                char target[256],joined[512];size_t length=0;
                error=ext4_readlink(prefix,target,255,&length);if(error)return -error;target[length]=0;
                u64 base=0;if(target[0]!='/'){base=end;while(base&&out[base-1]!='/')base--;memcpy(joined,out,base);}
                u64 suffix=ns_length(out+end);if(base+length+suffix>=sizeof(joined))return -36;
                memcpy(joined+base,target,length);memcpy(joined+base+length,out+end,suffix+1);
                if(!native_path(out,"/",joined))return -36;changed=1;break;
            }
            if(!out[end])return 0;
        }
        if(!changed)return 0;
    }
    return -40;
}
static int ext2_device_open(struct ext4_blockdev *b){(void)b;return 0;}
static int ext2_device_read(struct ext4_blockdev *b,void *out,uint64_t sector,uint32_t count){
    (void)b;if(sector>=native_partition_sectors||count>native_partition_sectors-sector)return 5;
    if(virtio_ready){for(u32 i=0;i<count;){u32 n=count-i;if(n>virtio_slots*VIRTIO_SLOT_SECTORS)n=virtio_slots*VIRTIO_SLOT_SECTORS;if(!virtio_transfer(native_partition_base+sector+i,(u8 *)out+i*512,n,0))return 5;i+=n;}return 0;}
    for(u32 i=0;i<count;i++)if(!native_disk((u32)sector+i,(u8 *)out+i*512,0))return 5;
    return 0;
}
static int ext2_device_write(struct ext4_blockdev *b,const void *in,uint64_t sector,uint32_t count){
    (void)b;if(sector>=native_partition_sectors||count>native_partition_sectors-sector)return 5;
    if(virtio_ready){for(u32 i=0;i<count;){u32 n=count-i;if(n>virtio_slots*VIRTIO_SLOT_SECTORS)n=virtio_slots*VIRTIO_SLOT_SECTORS;if(!virtio_transfer(native_partition_base+sector+i,(u8 *)in+i*512,n,1))return 5;i+=n;}return 0;}
    for(u32 i=0;i<count;i++)if(!native_disk((u32)sector+i,(u8 *)in+i*512,1))return 5;
    return 0;
}
EXT4_BLOCKDEV_STATIC_INSTANCE(ext2_device,512,1048576,ext2_device_open,ext2_device_read,ext2_device_write,ext2_device_open,0,0);
/* Superblock state follows Linux: mounting marks the volume in use, sync/fsync
 * flush the cache and mark it clean, and the first mutation after that marks
 * it in use again. A boot that finds the in-use mark reports an unclean stop. */
static int ext2_clean;
volatile u64 ext2_unclean_mounts,ext2_dirty_marks;
static int ext2_state(u16 state){struct ext4_sblock *sb=0;int error=ext4_get_sblock("/",&sb);if(error)return error;sb->state=state;return ext4_sb_write(&ext2_device,sb);}
static int ext2_sync(void){int error=ext4_cache_flush("/");if(!error)error=ext2_state(1);if(!error)ext2_clean=1;return error;}
static void ext2_dirty(void){if(ext2_ready&&ext2_clean){ext2_clean=0;ext2_dirty_marks++;ext2_state(2);}}
/* The superblock's free counts are only authoritative after a clean unmount,
 * as on Linux; lwext4 maintains them in memory and writes them back with the
 * state. Recompute them from the group descriptors at mount so an unclean stop
 * does not leave stale totals behind. */
static int ext2_recount(void){
    struct ext4_sblock *sb=0;int error=ext4_get_sblock("/",&sb);if(error)return error;
    u32 groups=ext4_block_group_cnt(sb),size=ext4_sb_get_desc_size(sb),block=ext4_sb_get_block_size(sb);
    u64 table=(u64)(to_le32(sb->first_data_block)+1)*block,free_blocks=0,free_inodes=0;u8 descriptor[64];
    for(u32 g=0;g<groups;g++){
        error=ext4_block_readbytes(&ext2_device,table+(u64)g*size,descriptor,size<64?size:64);if(error)return error;
        free_blocks+=*(u16 *)(descriptor+12);free_inodes+=*(u16 *)(descriptor+14);
        if(size>=64){free_blocks+=(u64)*(u16 *)(descriptor+32)<<16;free_inodes+=(u64)*(u16 *)(descriptor+34)<<16;}
    }
    ext4_sb_set_free_blocks_cnt(sb,free_blocks);sb->free_inodes_count=(u32)free_inodes;return 0;
}
static void ext2_init(void){
    if(!native_disk(2,native_sector,0)||*(u16 *)(native_sector+56)!=0xef53)return;
    if(*(u16 *)(native_sector+58)!=1){ext2_unclean_mounts++;serial("EXT2: previous session did not unmount cleanly\r\n");}
    ext2_device.bdif->ph_bcnt=native_partition_sectors;ext2_device.part_size=(u64)native_partition_sectors*512;
    int error=ext4_device_register(&ext2_device,"development");if(!error)error=ext4_mount("development","/",0);
    if(error){serial("EXT2 mount failed error=");hex(error);serial("\r\n");return;}
    error=ext2_recount();if(error){serial("EXT2 recount failed error=");hex(error);serial("\r\n");}
    ext2_ready=native_ready=1;native_count=0;serial("EXT2: writable development volume mounted\r\n");
}
static int ext2_find(const char *path){
    uint32_t mode;int error=ext4_mode_get(path,&mode);if(error)return -error;
    u32 index;for(index=0;index<native_count;index++)if(!NFILES[index].kind)break;
    if(index==NATIVE_FILE_CACHE)return -28;if(index==native_count)native_count++;
    NativeFile *f=&NFILES[index];memset(f,0,sizeof(*f));ns_copy(f->path,path);f->kind=(mode&0170000)==0040000?2:(mode&0170000)==0120000?3:1;
    *(u32 *)f->pad=mode&07777;*(u32 *)(f->pad+4)=1;
    if((mode&0170000)==0100000){ext4_file file;error=ext4_fopen(&file,path,"r");if(error){f->kind=0;return -error;}f->size=ext4_fsize(&file);ext4_fclose(&file);}
    if(f->kind==3){char target[256];size_t size=0;error=ext4_readlink(path,target,sizeof(target),&size);if(error){f->kind=0;return -error;}f->size=size;}
    return (int)index;
}
static int ext2_grow(ext4_file *file,u64 size){
    if(size>512ULL*1024*1024)return 27;
    u8 zeros[512];memset(zeros,0,sizeof(zeros));int error=ext4_fseek(file,ext4_fsize(file),0);
    while(!error&&ext4_fsize(file)<size){u64 amount=size-ext4_fsize(file);if(amount>sizeof(zeros))amount=sizeof(zeros);size_t done=0;
        error=ext4_fwrite(file,zeros,amount,&done);if(!error&&done!=amount)error=28;}
    return error;
}
static i64 ext2_io(int index,u64 offset,void *buffer,u64 count,int write){
    ext4_file file;int error=ext4_fopen(&file,NFILES[index].path,write?"r+":"r");if(error)return -error;
    if(!write&&offset>=ext4_fsize(&file)){ext4_fclose(&file);return 0;}
    size_t done=0;if(write&&offset>ext4_fsize(&file))error=ext2_grow(&file,offset);
    if(!error)error=ext4_fseek(&file,offset,0);
    if(!error)error=write?ext4_fwrite(&file,buffer,count,&done):ext4_fread(&file,buffer,count,&done);
    NFILES[index].size=ext4_fsize(&file);int close_error=ext4_fclose(&file);
    if(write&&!error&&!close_error){ext4_mtime_set(NFILES[index].path,native_timestamp());ext4_ctime_set(NFILES[index].path,native_timestamp());}
    if(write&&(error||close_error||done!=count)){serial("EXT2 write error=");hex(error?error:close_error);serial(" offset=");hex(offset);serial(" requested=");hex(count);serial(" completed=");hex(done);serial("\r\n");}
    if(error)return -error;if(close_error)return -close_error;return done;
}
static int ext2_commit(int index){
    NativeFile *f=&NFILES[index];if(!f->kind)return ext4_fremove(f->path)==0;
    int error=ext4_mode_set(f->path,*(u32 *)f->pad);
    if(!error&&f->kind==1){ext4_file file;error=ext4_fopen(&file,f->path,"r+");if(!error){error=f->size>ext4_fsize(&file)?ext2_grow(&file,f->size):ext4_ftruncate(&file,f->size);ext4_fclose(&file);}}
    return error==0;
}
static int ext2_create(const char *path){
    char resolved[256];int resolve=ext2_resolve(resolved,path,0);if(resolve)return resolve;path=resolved;
    ext4_file file;int error=ext4_fopen(&file,path,"w+");if(error)return -error;ext4_fclose(&file);return ext2_find(path);
}
