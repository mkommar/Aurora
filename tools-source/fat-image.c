#include <ff.h>
#include <diskio.h>
typedef int (*fat_callback)(void *,unsigned long long,unsigned int,int);
static fat_callback transfer;
DSTATUS disk_initialize(BYTE drive){return drive?STA_NOINIT:0;}
DSTATUS disk_status(BYTE drive){return disk_initialize(drive);}
DRESULT disk_read(BYTE drive,BYTE *buffer,LBA_t sector,UINT count){return drive||transfer(buffer,sector,count,0)?RES_ERROR:RES_OK;}
DRESULT disk_write(BYTE drive,const BYTE *buffer,LBA_t sector,UINT count){return drive||transfer((void *)buffer,sector,count,1)?RES_ERROR:RES_OK;}
DRESULT disk_ioctl(BYTE drive,BYTE command,void *buffer){
    if(drive)return RES_PARERR;if(command==CTRL_SYNC)return RES_OK;
    if(command==GET_SECTOR_COUNT){*(LBA_t *)buffer=262144;return RES_OK;}
    if(command==GET_BLOCK_SIZE){*(DWORD *)buffer=1;return RES_OK;}return RES_PARERR;
}
__declspec(dllexport) int au_fatformat(fat_callback callback){
    transfer=callback;static unsigned char work[4096];MKFS_PARM options={.fmt=FM_FAT32|FM_SFD,.n_fat=2,.au_size=512};
    FRESULT result=f_mkfs("",&options,work,sizeof(work));if(result)return result;
    static FATFS volume;result=f_mount(&volume,"",1);if(result)return result;
    FIL file;result=f_open(&file,"README.TXT",FA_CREATE_ALWAYS|FA_WRITE);
    if(!result){const char message[]="Aurora FAT32 exchange volume. Mounted at /exchange.\r\n";UINT written;result=f_write(&file,message,sizeof(message)-1,&written);f_close(&file);}
    f_mount(0,"",0);return result;
}
