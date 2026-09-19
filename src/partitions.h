/* Partition discovery with GPT recovery. Formatting stays in the host image
 * tool, but when one GPT copy (header or table) fails its CRCs the other copy
 * is used and rewritten over the damaged one, as gdisk would do. */
static u32 native_partition_base,native_partition_sectors=1048576,fat_partition_base,fat_partition_sectors;
static u64 native_disk_sectors;
volatile u64 gpt_backup_recoveries,gpt_repairs,gpt_damaged_copies;
static u32 partition_crc(const u8 *bytes,u32 count){u32 crc=~0U;while(count--){crc^=*bytes++;for(int i=0;i<8;i++)crc=(crc>>1)^((0U-(crc&1))&0xedb88320U);}return ~crc;}
static int native_raw_disk(u32,void *,int);
/* Scratch below the native page pool, which is initialised after mounting. */
#define GPT_PRIMARY_ENTRIES ((u8 *)0x0e000000)
#define GPT_BACKUP_ENTRIES ((u8 *)0x0e010000)
static u32 gpt_table_sectors(const u8 *header){return (*(u32 *)(header+80)*128+511)/512;}
/* Load the header at `lba` and its table into `entries`; 1 when both validate. */
static int gpt_load(u64 lba,u8 *header,u8 *entries){
    if(!lba||lba>=0x10000000||!native_raw_disk((u32)lba,header,0))return 0;
    for(int i=0;i<8;i++)if(header[i]!=(u8)"EFI PART"[i])return 0;
    u32 size=*(u32 *)(header+12),expected=*(u32 *)(header+16);if(size<92||size>512)return 0;
    *(u32 *)(header+16)=0;u32 actual=partition_crc(header,size);*(u32 *)(header+16)=expected;
    if(actual!=expected||*(u64 *)(header+24)!=lba)return 0;
    u64 table=*(u64 *)(header+72),first=*(u64 *)(header+40),last=*(u64 *)(header+48);
    u32 count=*(u32 *)(header+80),entry_size=*(u32 *)(header+84),crc=*(u32 *)(header+88);
    if(!count||count>128||entry_size!=128||table>0x0fffff00||first>last||last>=0x10000000)return 0;
    for(u32 i=0;i<gpt_table_sectors(header);i++)if(!native_raw_disk((u32)table+i,entries+i*512,0))return 0;
    return partition_crc(entries,count*128)==crc;
}
/* Rewrite the copy at `lba` (table at `table`) from a validated header. */
static void gpt_repair(const u8 *good,const u8 *entries,u64 lba,u64 alternate,u64 table,const char *message){
    gpt_damaged_copies++;u8 header[512];memcpy(header,good,512);
    if(!lba||lba>=0x10000000||!table||table>=0x10000000)return;
    *(u64 *)(header+24)=lba;*(u64 *)(header+32)=alternate;*(u64 *)(header+72)=table;
    *(u32 *)(header+16)=0;*(u32 *)(header+16)=partition_crc(header,*(u32 *)(header+12));
    for(u32 i=0;i<gpt_table_sectors(header);i++)if(!native_raw_disk((u32)table+i,(void *)(entries+i*512),1))return;
    if(!native_raw_disk((u32)lba,header,1))return;
    gpt_repairs++;serial(message);
}
static int native_partitions_init(void){
    u8 sector[512],primary[512],backup[512];if(!native_raw_disk(0,sector,0))return 0;
    if(sector[510]!=0x55||sector[511]!=0xaa)return 1;
    int protective=0;
    for(int i=0;i<4;i++){u8 *entry=sector+446+i*16;u32 start=*(u32 *)(entry+8),count=*(u32 *)(entry+12);
        if(entry[4]==0xee)protective=1;
        if(!start||!count||start>=0x10000000||count>0x10000000-start)continue;
        if(entry[4]==0x83&&!native_partition_base){native_partition_base=start;native_partition_sectors=count;}
        if((entry[4]==0xb||entry[4]==0xc)&&!fat_partition_base){fat_partition_base=start;fat_partition_sectors=count;}
    }
    if(!protective)return native_partition_base!=0;
    native_partition_base=fat_partition_base=0;
    int primary_ok=gpt_load(1,primary,GPT_PRIMARY_ENTRIES);
    /* Without a readable primary header the backup sits in the last sector. */
    u64 alternate=primary_ok?*(u64 *)(primary+32):native_disk_sectors?native_disk_sectors-1:0;
    int backup_ok=alternate>1&&gpt_load(alternate,backup,GPT_BACKUP_ENTRIES);
    const u8 *header,*entries;
    if(primary_ok){header=primary;entries=GPT_PRIMARY_ENTRIES;
        if(!backup_ok)gpt_repair(primary,entries,alternate,1,alternate-gpt_table_sectors(primary),"GPT: backup header damaged; rewritten from primary\r\n");
    }else if(backup_ok){header=backup;entries=GPT_BACKUP_ENTRIES;gpt_backup_recoveries++;
        serial("GPT: primary header damaged; recovered from backup\r\n");
        gpt_repair(backup,entries,1,alternate,2,"GPT: primary header rewritten from backup\r\n");
    }else{serial("GPT: both headers damaged\r\n");return 0;}
    u64 first=*(u64 *)(header+40),last=*(u64 *)(header+48);u32 count=*(u32 *)(header+80);
    const u8 linux_type[16]={0xaf,0x3d,0xc6,0x0f,0x83,0x84,0x72,0x47,0x8e,0x79,0x3d,0x69,0xd8,0x47,0x7d,0xe4};
    const u8 fat_type[16]={0xa2,0xa0,0xd0,0xeb,0xe5,0xb9,0x33,0x44,0x87,0xc0,0x68,0xb6,0xb7,0x26,0x99,0xc7};
    for(u32 i=0;i<count;i++){const u8 *entry=entries+i*128;u64 start=*(u64 *)(entry+32),end=*(u64 *)(entry+40);
        if(start<first||end<start||end>last)continue;int linux_match=1,fat_match=1;
        for(int n=0;n<16;n++){if(entry[n]!=linux_type[n])linux_match=0;if(entry[n]!=fat_type[n])fat_match=0;}
        if(linux_match&&!native_partition_base){native_partition_base=start;native_partition_sectors=end-start+1;}
        if(fat_match&&!fat_partition_base){fat_partition_base=start;fat_partition_sectors=end-start+1;}
    }
    if(native_partition_base){serial("GPT: development and exchange partitions discovered\r\n");return 1;}return 0;
}
