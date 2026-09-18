/* Read-only partition discovery; formatting is confined to the host image tool. */
static u32 native_partition_base,native_partition_sectors=1048576,fat_partition_base,fat_partition_sectors;
static u32 partition_crc(const u8 *bytes,u32 count){u32 crc=~0U;while(count--){crc^=*bytes++;for(int i=0;i<8;i++)crc=(crc>>1)^((0U-(crc&1))&0xedb88320U);}return ~crc;}
static int native_raw_disk(u32,void *,int);
static int native_partitions_init(void){
    u8 sector[512];if(!native_raw_disk(0,sector,0))return 0;
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
    if(!native_raw_disk(1,sector,0))return 0;
    for(int i=0;i<8;i++)if(sector[i]!=(u8)"EFI PART"[i])return 0;
    u32 size=*(u32 *)(sector+12),expected=*(u32 *)(sector+16);if(size<92||size>512)return 0;
    *(u32 *)(sector+16)=0;if(partition_crc(sector,size)!=expected)return 0;
    u64 table=*(u64 *)(sector+72),first=*(u64 *)(sector+40),last=*(u64 *)(sector+48);
    u32 count=*(u32 *)(sector+80),entry_size=*(u32 *)(sector+84),crc=*(u32 *)(sector+88);
    if(!count||count>128||entry_size!=128||table>0x0fffff00||first>last||last>=0x10000000)return 0;
    u8 *entries=(u8 *)0x0e000000;
    for(u32 i=0;i<(count*128+511)/512;i++)if(!native_raw_disk((u32)table+i,entries+i*512,0))return 0;
    if(partition_crc(entries,count*128)!=crc)return 0;
    const u8 linux_type[16]={0xaf,0x3d,0xc6,0x0f,0x83,0x84,0x72,0x47,0x8e,0x79,0x3d,0x69,0xd8,0x47,0x7d,0xe4};
    const u8 fat_type[16]={0xa2,0xa0,0xd0,0xeb,0xe5,0xb9,0x33,0x44,0x87,0xc0,0x68,0xb6,0xb7,0x26,0x99,0xc7};
    for(u32 i=0;i<count;i++){u8 *entry=entries+i*128;u64 start=*(u64 *)(entry+32),end=*(u64 *)(entry+40);
        if(start<first||end<start||end>last)continue;int linux_match=1,fat_match=1;
        for(int n=0;n<16;n++){if(entry[n]!=linux_type[n])linux_match=0;if(entry[n]!=fat_type[n])fat_match=0;}
        if(linux_match&&!native_partition_base){native_partition_base=start;native_partition_sectors=end-start+1;}
        if(fat_match&&!fat_partition_base){fat_partition_base=start;fat_partition_sectors=end-start+1;}
    }
    if(native_partition_base){serial("GPT: development and exchange partitions discovered\r\n");return 1;}return 0;
}
