/* Host-only artifact access using the same pinned filesystem implementation. */
#include <ext4.h>
#include <ext4_mkfs.h>
#include <ext4_super.h>
#include <ext4_blockdev.h>
typedef int (*sector_callback)(void *,uint64_t,uint32_t,int);
static sector_callback transfer;
static int opened(struct ext4_blockdev *b){(void)b;return 0;}
static int read_blocks(struct ext4_blockdev *b,void *data,uint64_t sector,uint32_t count){(void)b;return transfer(data,sector,count,0);}
static int write_blocks(struct ext4_blockdev *b,const void *data,uint64_t sector,uint32_t count){(void)b;return transfer((void *)data,sector,count,1);}
EXT4_BLOCKDEV_STATIC_INSTANCE(device,512,1048576,opened,read_blocks,write_blocks,opened,0,0);
/* Attach an existing image without formatting it. The host bounds every I/O. */
__declspec(dllexport) void au_attach(sector_callback callback){transfer=callback;}
__declspec(dllexport) int au_format(sector_callback callback){
    transfer=callback;static struct ext4_fs fs;
    struct ext4_mkfs_info info={.len=512ULL*1024*1024,.block_size=4096,.inodes=32768,.label="Aurora development"};
    return ext4_mkfs(&fs,&device,&info,2);
}
/* Superblock free counts are only authoritative after a clean unmount; rebuild
 * them from the group descriptors so a staged image never carries stale totals. */
static int recount(void){
    struct ext4_sblock *sb=0;int r=ext4_get_sblock("/",&sb);if(r)return r;
    uint32_t groups=ext4_block_group_cnt(sb),size=ext4_sb_get_desc_size(sb),block=ext4_sb_get_block_size(sb);
    uint64_t table=(uint64_t)(to_le32(sb->first_data_block)+1)*block,free_blocks=0,free_inodes=0;uint8_t d[64];
    for(uint32_t g=0;g<groups;g++){
        r=ext4_block_readbytes(&device,table+(uint64_t)g*size,d,size<64?size:64);if(r)return r;
        free_blocks+=*(uint16_t *)(d+12);free_inodes+=*(uint16_t *)(d+14);
        if(size>=64){free_blocks+=(uint64_t)*(uint16_t *)(d+32)<<16;free_inodes+=(uint64_t)*(uint16_t *)(d+34)<<16;}
    }
    ext4_sb_set_free_blocks_cnt(sb,free_blocks);sb->free_inodes_count=(uint32_t)free_inodes;return 0;
}
__declspec(dllexport) int au_mount(void){int r=ext4_device_register(&device,"image");if(!r)r=ext4_mount("image","/",0);if(!r)r=recount();if(!r)r=ext4_cache_write_back("/",1);return r;}
__declspec(dllexport) int au_mount_readonly(void){int r=ext4_device_register(&device,"image");return r?r:ext4_mount("image","/",1);}
__declspec(dllexport) int au_get(const char *path,void *data,uint64_t capacity,uint64_t *size){
    ext4_file f;int r=ext4_fopen(&f,path,"r");if(r)return r;*size=ext4_fsize(&f);size_t done=0;
    if(data)r=capacity<*size?27:ext4_fread(&f,data,*size,&done);
    int close=ext4_fclose(&f);return r?r:close?close:data&&done!=*size?5:0;
}
__declspec(dllexport) int au_mkdir(const char *path){return ext4_dir_mk(path);}
__declspec(dllexport) int au_put(const char *path,const void *data,uint64_t size,uint32_t mode){
    ext4_file f;int r=ext4_fopen(&f,path,"w");if(r)return r;size_t done=0;
    if(size)r=ext4_fwrite(&f,data,size,&done);int close=ext4_fclose(&f);if(r)return r;if(close)return close;
    if(done!=size)return 5;return ext4_mode_set(path,mode);
}
__declspec(dllexport) int au_symlink(const char *path,const char *target){return ext4_fsymlink(target,path);}
__declspec(dllexport) int au_close(void){int r=ext4_cache_flush("/");int close=ext4_umount("/");return r?r:close;}
