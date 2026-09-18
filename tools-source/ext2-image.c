/* Host-only image writer using the same pinned filesystem implementation. */
#include <ext4.h>
#include <ext4_mkfs.h>
typedef int (*sector_callback)(void *,uint64_t,uint32_t,int);
static sector_callback transfer;
static int opened(struct ext4_blockdev *b){(void)b;return 0;}
static int read_blocks(struct ext4_blockdev *b,void *data,uint64_t sector,uint32_t count){(void)b;return transfer(data,sector,count,0);}
static int write_blocks(struct ext4_blockdev *b,const void *data,uint64_t sector,uint32_t count){(void)b;return transfer((void *)data,sector,count,1);}
EXT4_BLOCKDEV_STATIC_INSTANCE(device,512,1048576,opened,read_blocks,write_blocks,opened,0,0);
__declspec(dllexport) int au_format(sector_callback callback){
    transfer=callback;static struct ext4_fs fs;
    struct ext4_mkfs_info info={.len=512ULL*1024*1024,.block_size=4096,.inodes=32768,.label="Aurora development"};
    return ext4_mkfs(&fs,&device,&info,2);
}
__declspec(dllexport) int au_mount(void){int r=ext4_device_register(&device,"image");if(!r)r=ext4_mount("image","/",0);if(!r)r=ext4_cache_write_back("/",1);return r;}
__declspec(dllexport) int au_mkdir(const char *path){return ext4_dir_mk(path);}
__declspec(dllexport) int au_put(const char *path,const void *data,uint64_t size,uint32_t mode){
    ext4_file f;int r=ext4_fopen(&f,path,"w");if(r)return r;size_t done=0;
    if(size)r=ext4_fwrite(&f,data,size,&done);int close=ext4_fclose(&f);if(r)return r;if(close)return close;
    if(done!=size)return 5;return ext4_mode_set(path,mode);
}
__declspec(dllexport) int au_symlink(const char *path,const char *target){return ext4_fsymlink(target,path);}
__declspec(dllexport) int au_close(void){int r=ext4_cache_flush("/");int close=ext4_umount("/");return r?r:close;}
