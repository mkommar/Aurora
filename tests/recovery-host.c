/* Compile the production recovery code with an in-memory block device. */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
static void *memcpy(void *out,const void *in,u64 n){u8 *d=out;const u8 *s=in;while(n--)*d++=*s++;return out;}
static void serial(const char *s){(void)s;}
static int (*transfer)(u32,void *,int);
static int native_raw_disk(u32 sector,void *data,int write){return transfer(sector,data,write);}
static int native_disk_flush(void){return transfer(0,0,2);}
static u8 primary_entries[16384],backup_entries[16384];
#define GPT_PRIMARY_ENTRIES primary_entries
#define GPT_BACKUP_ENTRIES backup_entries
#include "../src/partitions.h"
__declspec(dllexport) void recovery_setup(u64 sectors,int (*callback)(u32,void *,int)){
    native_disk_sectors=sectors;transfer=callback;native_partition_base=fat_partition_base=0;
    gpt_backup_recoveries=gpt_repairs=gpt_damaged_copies=gpt_repair_failures=0;
}
__declspec(dllexport) int recovery_gpt(void){return native_partitions_init();}
__declspec(dllexport) u32 recovery_partition_base(void){return native_partition_base;}
__declspec(dllexport) u32 recovery_partition_sectors(void){return native_partition_sectors;}
__declspec(dllexport) u32 recovery_fat_base(void){return fat_partition_base;}
__declspec(dllexport) u32 recovery_fat_sectors(void){return fat_partition_sectors;}
__declspec(dllexport) u64 recovery_count(int which){return which==0?gpt_repairs:which==1?gpt_repair_failures:gpt_backup_recoveries;}
static int ext2_ready=1,ext2_device;
struct ext4_sblock {u16 state;};
static struct ext4_sblock sb;
static int ext4_get_sblock(const char *path,struct ext4_sblock **out){(void)path;*out=&sb;return 0;}
static int ext4_sb_write(void *device,struct ext4_sblock *state){(void)device;return transfer(state->state,0,3)?0:5;}
static int ext4_cache_flush(const char *path){(void)path;return transfer(0,0,4)?0:5;}
#include "../src/ext2_recovery.h"
__declspec(dllexport) void recovery_ext2_reset(int clean){ext2_clean=clean;ext2_write_error=0;ext2_dirty_marks=ext2_sync_failures=0;sb.state=clean?1:2;}
__declspec(dllexport) int recovery_dirty(void){return ext2_dirty();}
__declspec(dllexport) int recovery_sync(void){return ext2_sync();}
__declspec(dllexport) int recovery_clean(void){return ext2_clean;}
