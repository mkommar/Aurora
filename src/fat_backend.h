#include <ff.h>
#include <diskio.h>
static FATFS fat_volume;
static int fat_ready;
static int fat_path(const char *path){const char *prefix="/exchange";for(int i=0;i<9;i++)if(path[i]!=prefix[i])return 0;return !path[9]||path[9]=='/';}
static const char *fat_name(const char *path){return path[9]?path+9:"/";}
DSTATUS disk_initialize(BYTE drive){return drive||!fat_partition_base?STA_NOINIT:0;}
DSTATUS disk_status(BYTE drive){return disk_initialize(drive);}
DRESULT disk_read(BYTE drive,BYTE *buffer,LBA_t sector,UINT count){
    if(drive||sector>=fat_partition_sectors||count>fat_partition_sectors-sector)return RES_PARERR;
    return native_raw_disk_range(fat_partition_base+sector,buffer,count,0)?RES_OK:RES_ERROR;
}
DRESULT disk_write(BYTE drive,const BYTE *buffer,LBA_t sector,UINT count){
    if(drive||sector>=fat_partition_sectors||count>fat_partition_sectors-sector)return RES_PARERR;
    return native_raw_disk_range(fat_partition_base+sector,(void *)buffer,count,1)?RES_OK:RES_ERROR;
}
DRESULT disk_ioctl(BYTE drive,BYTE command,void *buffer){
    if(drive)return RES_PARERR;
    if(command==CTRL_SYNC)return (virtio_present?virtio_transfer(0,0,0,4):disk_flush())?RES_OK:RES_ERROR;
    if(command==GET_SECTOR_COUNT){*(LBA_t *)buffer=fat_partition_sectors;return RES_OK;}
    if(command==GET_BLOCK_SIZE){*(DWORD *)buffer=1;return RES_OK;}return RES_PARERR;
}
static int fat_error(FRESULT error){return error==FR_NO_FILE||error==FR_NO_PATH?-2:error==FR_EXIST?-17:error==FR_DENIED?-13:error==FR_INVALID_NAME?-22:error==FR_NOT_ENOUGH_CORE?-12:-5;}
static void fat_init(void){if(fat_partition_base&&f_mount(&fat_volume,"",1)==FR_OK){fat_ready=1;serial("FAT32: exchange volume mounted at /exchange\r\n");}}
static int fat_find(const char *path){
    FILINFO info;memset(&info,0,sizeof(info));FRESULT error=FR_OK;
    if(!path[9]||(path[9]=='/'&&!path[10]))info.fattrib=AM_DIR;else error=f_stat(fat_name(path),&info);
    if(error)return fat_error(error);u32 index;for(index=0;index<native_count;index++)if(!NFILES[index].kind)break;
    if(index==NATIVE_FILE_CACHE)return -28;if(index==native_count)native_count++;
    NativeFile *f=&NFILES[index];memset(f,0,sizeof(*f));ns_copy(f->path,path);f->kind=(info.fattrib&AM_DIR)?2:1;f->size=info.fsize;*(u32 *)f->pad=0777;*(u32 *)(f->pad+4)=1;return index;
}
static int fat_create(const char *path){FIL file;FRESULT error=f_open(&file,fat_name(path),FA_WRITE|FA_CREATE_NEW);if(error)return fat_error(error);f_close(&file);return fat_find(path);}
static i64 fat_io(int index,u64 offset,void *buffer,u64 count,int write){
    FIL file;FRESULT error=f_open(&file,fat_name(NFILES[index].path),write?FA_WRITE:FA_READ);if(error)return fat_error(error);
    if(offset>0xffffffffULL||count>0xffffffffULL-offset){f_close(&file);return -27;}
    UINT done=0;error=f_lseek(&file,offset);if(!error)error=write?f_write(&file,buffer,count,&done):f_read(&file,buffer,count,&done);
    NFILES[index].size=f_size(&file);FRESULT close=f_close(&file);return error?fat_error(error):close?fat_error(close):(i64)done;
}
static int fat_commit(int index){NativeFile *f=&NFILES[index];if(!f->kind)return f_unlink(fat_name(f->path))==FR_OK;
    if(f->kind==2)return 1;FIL file;FRESULT error=f_open(&file,fat_name(f->path),FA_WRITE);if(error)return 0;
    error=f_lseek(&file,f->size);if(!error)error=f_truncate(&file);FRESULT close=f_close(&file);return !error&&!close;
}
