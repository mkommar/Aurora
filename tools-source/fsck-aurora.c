/* fsck-aurora: read-only consistency checker that runs inside Aurora against
 * the raw devices the kernel exposes (/dev/disk for the development disk,
 * /dev/boot for the AuroraFS boot disk). It checks both GPT copies, the ext2
 * development volume, the FAT32 exchange volume and the AuroraFS directory.
 *
 * Build inside Aurora:  gcc -O2 fsck-aurora.c -o fsck-aurora
 * Exit status follows fsck: 0 clean, 1 accounting problems that a repairing
 * checker would fix without data loss (free counts, lost blocks, orphans),
 * 4 structural corruption (bad pointers, cross-links, broken directories). */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;
static int warnings,errors,verbose;
static void warn(const char *fmt,...) __attribute__((format(printf,1,2)));
static void fail(const char *fmt,...) __attribute__((format(printf,1,2)));
#include <stdarg.h>
static void warn(const char *fmt,...){va_list ap;va_start(ap,fmt);printf("  warning: ");vprintf(fmt,ap);printf("\n");va_end(ap);warnings++;}
static void fail(const char *fmt,...){va_list ap;va_start(ap,fmt);printf("  ERROR: ");vprintf(fmt,ap);printf("\n");va_end(ap);errors++;}
static int readat(int fd,u64 offset,void *buffer,size_t size){
    size_t done=0;while(done<size){ssize_t n=pread(fd,(u8 *)buffer+done,size-done,offset+done);if(n<=0)return 0;done+=n;}return 1;
}
static u16 le16(const u8 *p){return p[0]|(p[1]<<8);}
static u32 le32(const u8 *p){return p[0]|(p[1]<<8)|(p[2]<<16)|((u32)p[3]<<24);}
static u64 le64(const u8 *p){return le32(p)|((u64)le32(p+4)<<32);}
static u32 crc32(const u8 *bytes,size_t count){u32 crc=~0U;while(count--){crc^=*bytes++;for(int i=0;i<8;i++)crc=(crc>>1)^((0U-(crc&1))&0xedb88320U);}return ~crc;}

/* ---------------------------------------------------------------- GPT --- */
typedef struct {u64 start,end;int kind;} Partition; /* kind 1 ext2, 2 fat */
static int gpt_load(int fd,u64 lba,u8 *header,u8 **entries,const char *label){
    if(!readat(fd,lba*512,header,512)){fail("%s GPT header at LBA %llu unreadable",label,(unsigned long long)lba);return 0;}
    if(memcmp(header,"EFI PART",8)){fail("%s GPT header at LBA %llu has no signature",label,(unsigned long long)lba);return 0;}
    u32 size=le32(header+12),expected=le32(header+16);if(size<92||size>512){fail("%s GPT header size %u",label,size);return 0;}
    u8 copy[512];memcpy(copy,header,512);memset(copy+16,0,4);
    if(crc32(copy,size)!=expected){fail("%s GPT header CRC mismatch",label);return 0;}
    if(le64(header+24)!=lba){fail("%s GPT header claims LBA %llu",label,(unsigned long long)le64(header+24));return 0;}
    u32 count=le32(header+80),entry=le32(header+84);u64 table=le64(header+72);
    if(!count||count>128||entry!=128){fail("%s GPT table geometry %u x %u",label,count,entry);return 0;}
    *entries=malloc(count*128);if(!readat(fd,table*512,*entries,count*128)){fail("%s GPT table unreadable",label);return 0;}
    if(crc32(*entries,count*128)!=le32(header+88)){fail("%s GPT table CRC mismatch",label);return 0;}
    return 1;
}
static int check_gpt(int fd,u64 sectors,Partition *out){
    printf("GPT: checking both header copies\n");u8 mbr[512];if(!readat(fd,0,mbr,512)){fail("MBR unreadable");return 0;}
    int protective=0;for(int i=0;i<4;i++)if(mbr[446+i*16+4]==0xee)protective=1;
    if(!protective){warn("no protective MBR entry");}
    u8 primary[512],backup[512],*primary_entries=0,*backup_entries=0;
    int primary_ok=gpt_load(fd,1,primary,&primary_entries,"primary");
    u64 alternate=primary_ok?le64(primary+32):sectors-1;
    int backup_ok=gpt_load(fd,alternate,backup,&backup_entries,"backup");
    if(primary_ok&&backup_ok){
        if(le64(backup+32)!=1)fail("backup header points its alternate at LBA %llu",(unsigned long long)le64(backup+32));
        if(memcmp(primary+56,backup+56,16))fail("disk GUIDs differ between GPT copies");
        if(le32(primary+80)!=le32(backup+80)||memcmp(primary_entries,backup_entries,le32(primary+80)*128))fail("partition tables differ between GPT copies");
        if(alternate!=sectors-1)warn("backup header at LBA %llu, disk ends at %llu",(unsigned long long)alternate,(unsigned long long)(sectors-1));
    }
    const u8 *header=primary_ok?primary:backup_ok?backup:0,*entries=primary_ok?primary_entries:backup_entries;
    if(!header){fail("no usable GPT copy");return 0;}
    static const u8 linux_type[16]={0xaf,0x3d,0xc6,0x0f,0x83,0x84,0x72,0x47,0x8e,0x79,0x3d,0x69,0xd8,0x47,0x7d,0xe4};
    static const u8 fat_type[16]={0xa2,0xa0,0xd0,0xeb,0xe5,0xb9,0x33,0x44,0x87,0xc0,0x68,0xb6,0xb7,0x26,0x99,0xc7};
    u64 first=le64(header+40),last=le64(header+48);u32 count=le32(header+80);int found=0;
    for(u32 i=0;i<count;i++){const u8 *e=entries+i*128;u64 start=le64(e+32),end=le64(e+40);int empty=1;for(int n=0;n<16;n++)if(e[n])empty=0;if(empty)continue;
        if(start<first||end<start||end>last){fail("partition %u [%llu,%llu] outside usable range",i,(unsigned long long)start,(unsigned long long)end);continue;}
        for(int j=0;j<found;j++)if(start<=out[j].end&&out[j].start<=end)fail("partition %u overlaps another",i);
        if(found<8){out[found].start=start;out[found].end=end;out[found].kind=!memcmp(e,linux_type,16)?1:!memcmp(e,fat_type,16)?2:0;found++;}
    }
    printf("GPT: %d partitions, primary %s, backup %s\n",found,primary_ok?"valid":"DAMAGED",backup_ok?"valid":"DAMAGED");
    free(primary_entries);free(backup_entries);return found;
}

/* --------------------------------------------------------------- ext2 --- */
typedef struct {int fd;u64 base;u32 block_size,blocks,inodes,per_group,inodes_per_group,first_data,inode_size,groups,gdt_blocks,reserved_gdt;int sparse;
    u8 *bmap,*imap,*used,*seen;u16 *links;u8 *gdt;u64 orphans,dirs;} Ext2;
static int bit(const u8 *map,u64 n){return (map[n/8]>>(n%8))&1;}
static void setbit(u8 *map,u64 n){map[n/8]|=1<<(n%8);}
static int ext2_read_block(Ext2 *e,u64 block,void *buffer){return readat(e->fd,e->base+block*e->block_size,buffer,e->block_size);}
static int ext2_has_backup(Ext2 *e,u32 group){if(!e->sparse||group<=1)return 1;for(u32 p=3;p<=group;p*=3)if(p==group)return 1;for(u32 p=5;p<=group;p*=5)if(p==group)return 1;for(u32 p=7;p<=group;p*=7)if(p==group)return 1;return 0;}
static void ext2_claim(Ext2 *e,u64 block,const char *what,u32 inode){
    if(block<e->first_data||block>=e->blocks){fail("inode %u: %s block %llu out of range",inode,what,(unsigned long long)block);return;}
    if(bit(e->used,block)){fail("inode %u: %s block %llu is cross-linked",inode,what,(unsigned long long)block);return;}
    setbit(e->used,block);
}
static u64 ext2_walk_indirect(Ext2 *e,u32 block,int level,u32 inode,void (*visit)(Ext2 *,u32,u32,void *),void *context){
    if(!block)return 0;ext2_claim(e,block,"indirect",inode);u64 count=1;u32 *table=malloc(e->block_size);
    if(!ext2_read_block(e,block,table)){fail("inode %u: indirect block %u unreadable",inode,block);free(table);return count;}
    for(u32 i=0;i<e->block_size/4;i++)if(table[i]){if(level==1){ext2_claim(e,table[i],"data",inode);count++;if(visit)visit(e,table[i],inode,context);}else count+=ext2_walk_indirect(e,table[i],level-1,inode,visit,context);}
    free(table);return count;
}
typedef struct {u32 inode;int first;} DirContext;
static void ext2_visit_dir_block(Ext2 *e,u32 block,u32 inode,void *context){
    DirContext *dc=context;u8 *data=malloc(e->block_size);if(!ext2_read_block(e,block,data)){fail("directory %u: block %u unreadable",inode,block);free(data);return;}
    u32 offset=0;int index=0;
    while(offset<e->block_size){
        if(offset+8>e->block_size){fail("directory %u: entry overruns block %u",inode,block);break;}
        u32 target=le32(data+offset);u16 rec=le16(data+offset+4);u8 name_len=data[offset+6];
        if(rec<8||(rec&3)||offset+rec>e->block_size){fail("directory %u: bad record length %u at offset %u",inode,rec,offset);break;}
        if(target){
            if(name_len+8>rec){fail("directory %u: name overruns record",inode);break;}
            if(target>e->inodes){fail("directory %u: entry refers to inode %u beyond %u",inode,target,e->inodes);}
            else{e->links[target]++;
                if(dc->first&&index==0&&!(name_len==1&&data[offset+8]=='.'))fail("directory %u: first entry is not '.'",inode);
                if(dc->first&&index==1&&!(name_len==2&&data[offset+8]=='.'&&data[offset+9]=='.'))fail("directory %u: second entry is not '..'",inode);
                if(inode==2&&name_len==31&&!memcmp(data+offset+8,".aurora-orphan-",15)){e->orphans++;if(verbose)printf("  orphan pending reclaim: %.31s\n",data+offset+8);}
                if(!bit(e->imap,target-1))fail("directory %u: entry '%.*s' refers to free inode %u",inode,name_len,data+offset+8,target);
            }
        }
        offset+=rec;index++;
    }
    dc->first=0;free(data);
}
static int check_ext2(int fd,u64 base,u64 sectors){
    Ext2 e={0};e.fd=fd;e.base=base;u8 sb[1024];
    if(!readat(fd,base+1024,sb,1024)){fail("ext2 superblock unreadable");return 4;}
    if(le16(sb+56)!=0xef53){fail("ext2 superblock has no magic");return 4;}
    e.block_size=1024u<<le32(sb+24);e.blocks=le32(sb+4);e.inodes=le32(sb+0);e.per_group=le32(sb+32);e.inodes_per_group=le32(sb+40);e.first_data=le32(sb+20);
    u32 rev=le32(sb+76);e.inode_size=rev>=1?le16(sb+88):128;e.reserved_gdt=le16(sb+206);e.sparse=(le32(sb+100)&1)!=0;u16 state=le16(sb+58);
    u32 features_incompat=le32(sb+96);
    printf("ext2: %u blocks of %u bytes, %u inodes, state %s\n",e.blocks,e.block_size,e.inodes,state==1?"clean":state==2?"in use / not cleanly unmounted":"unknown");
    if(state!=1)warn("superblock state %u: the previous session did not unmount cleanly",state);
    if(features_incompat&~0x2u)warn("incompatible features 0x%x beyond filetype",features_incompat);
    if(e.block_size>65536||!e.per_group||!e.inodes_per_group||(u64)e.blocks*e.block_size>sectors*512){fail("ext2 geometry inconsistent with partition");return 4;}
    e.groups=(e.blocks-e.first_data+e.per_group-1)/e.per_group;e.gdt_blocks=(e.groups*32+e.block_size-1)/e.block_size;
    e.gdt=malloc(e.gdt_blocks*e.block_size);for(u32 i=0;i<e.gdt_blocks;i++)if(!ext2_read_block(&e,e.first_data+1+i,e.gdt+i*e.block_size)){fail("group descriptors unreadable");return 4;}
    size_t bmap_bytes=(e.blocks+7)/8,imap_bytes=(e.inodes+7)/8;
    e.bmap=calloc(1,bmap_bytes);e.imap=calloc(1,imap_bytes);e.used=calloc(1,bmap_bytes);e.seen=calloc(1,imap_bytes);e.links=calloc(e.inodes+1,2);
    u8 *scratch=malloc(e.block_size);
    for(u32 g=0;g<e.groups;g++){const u8 *d=e.gdt+g*32;u32 block_bitmap=le32(d),inode_bitmap=le32(d+4),table=le32(d+8);
        u32 table_blocks=(e.inodes_per_group*e.inode_size+e.block_size-1)/e.block_size;u64 group_first=e.first_data+(u64)g*e.per_group;
        if(ext2_has_backup(&e,g))for(u32 i=0;i<1+e.gdt_blocks+e.reserved_gdt;i++)if(group_first+i<e.blocks)setbit(e.used,group_first+i);
        ext2_claim(&e,block_bitmap,"block bitmap",0);ext2_claim(&e,inode_bitmap,"inode bitmap",0);
        for(u32 i=0;i<table_blocks;i++)ext2_claim(&e,table+i,"inode table",0);
        if(!ext2_read_block(&e,block_bitmap,scratch)){fail("group %u block bitmap unreadable",g);continue;}
        u64 remaining=e.blocks-group_first;u32 in_group=remaining<e.per_group?(u32)remaining:e.per_group;
        for(u32 i=0;i<in_group;i++)if(bit(scratch,i))setbit(e.bmap,group_first+i);
        if(!ext2_read_block(&e,inode_bitmap,scratch)){fail("group %u inode bitmap unreadable",g);continue;}
        for(u32 i=0;i<e.inodes_per_group&&(u64)g*e.inodes_per_group+i<e.inodes;i++)if(bit(scratch,i))setbit(e.imap,(u64)g*e.inodes_per_group+i);
    }
    /* Pass 1: every in-use inode claims its blocks; directories are parsed. */
    u8 *table_data=malloc((size_t)e.inodes_per_group*e.inode_size);u64 in_use=0;
    for(u32 g=0;g<e.groups;g++){u32 table=le32(e.gdt+g*32+8);
        if(!readat(fd,base+(u64)table*e.block_size,table_data,(size_t)e.inodes_per_group*e.inode_size)){fail("group %u inode table unreadable",g);continue;}
        for(u32 i=0;i<e.inodes_per_group;i++){u32 ino=g*e.inodes_per_group+i+1;if(ino>e.inodes)break;const u8 *in=table_data+(size_t)i*e.inode_size;
            u16 mode=le16(in),links=le16(in+26);u32 dtime=le32(in+20),blocks512=le32(in+28);u64 size=le32(in+4)|((u64)le32(in+108)<<32);
            int allocated=bit(e.imap,ino-1);
            if(!links||!mode){if(allocated&&ino>10)warn("inode %u marked allocated but unused (lost inode, dtime %u)",ino,dtime);continue;}
            if(!allocated){fail("inode %u in use (mode %o, %u links) but marked free",ino,mode,links);}
            setbit(e.seen,ino-1);in_use++;u32 type=mode&0170000;
            if(type!=0100000&&type!=0040000&&type!=0120000&&type!=0010000&&type!=0020000&&type!=0060000&&type!=0140000){fail("inode %u has invalid mode %o",ino,mode);continue;}
            int inline_symlink=type==0120000&&size<60&&!blocks512;if(inline_symlink||type==0010000||type==0020000||type==0060000||type==0140000)continue;
            DirContext dc={ino,1};void (*visit)(Ext2 *,u32,u32,void *)=type==0040000?ext2_visit_dir_block:0;u64 count=0;
            if(type==0040000)e.dirs++;
            for(int b=0;b<12;b++){u32 block=le32(in+40+b*4);if(!block)continue;ext2_claim(&e,block,"data",ino);count++;if(visit)visit(&e,block,ino,&dc);}
            count+=ext2_walk_indirect(&e,le32(in+88),1,ino,visit,&dc);
            count+=ext2_walk_indirect(&e,le32(in+92),2,ino,visit,&dc);
            count+=ext2_walk_indirect(&e,le32(in+96),3,ino,visit,&dc);
            if(count*(e.block_size/512)!=blocks512)fail("inode %u: i_blocks %u but %llu blocks referenced",ino,blocks512,(unsigned long long)count);
            (void)size;
        }
    }
    /* Pass 2: link counts and bitmaps against what the tree references. Every
       directory entry, including '.' and '..', is one link. */
    u64 lost_blocks=0,free_blocks=0,free_inodes=0,unreferenced=0;
    for(u32 g=0;g<e.groups;g++){u32 table=le32(e.gdt+g*32+8);
        if(!readat(fd,base+(u64)table*e.block_size,table_data,(size_t)e.inodes_per_group*e.inode_size))continue;
        for(u32 i=0;i<e.inodes_per_group;i++){u32 ino=g*e.inodes_per_group+i+1;if(ino>e.inodes)break;if(!bit(e.seen,ino-1))continue;
            u16 links=le16(table_data+(size_t)i*e.inode_size+26);u16 found=e.links[ino];
            if(ino<=10&&ino!=2)continue; /* reserved inodes have no directory entries */
            if(!found){unreferenced++;warn("inode %u in use with %u links but no directory entry (orphan)",ino,links);}
            else if(found!=links)warn("inode %u: link count %u but %u directory entries",ino,links,found);
        }
    }
    for(u64 b=0;b<e.blocks;b++){int on_disk=b<e.first_data?1:bit(e.bmap,b),referenced=bit(e.used,b);
        if(b<e.first_data)continue;
        if(!on_disk)free_blocks++;
        if(on_disk&&!referenced)lost_blocks++;
        if(!on_disk&&referenced)fail("block %llu is referenced but marked free",(unsigned long long)b);
    }
    if(lost_blocks)warn("%llu blocks allocated but unreferenced (lost blocks)",(unsigned long long)lost_blocks);
    for(u32 ino=1;ino<=e.inodes;ino++)if(!bit(e.imap,ino-1))free_inodes++;
    u32 sb_free_blocks=le32(sb+12),sb_free_inodes=le32(sb+16);
    if(sb_free_blocks!=free_blocks)warn("superblock free blocks %u, bitmap says %llu",sb_free_blocks,(unsigned long long)free_blocks);
    if(sb_free_inodes!=free_inodes)warn("superblock free inodes %u, bitmap says %llu",sb_free_inodes,(unsigned long long)free_inodes);
    for(u32 g=0;g<e.groups;g++){const u8 *d=e.gdt+g*32;u64 group_first=e.first_data+(u64)g*e.per_group,remaining=e.blocks-group_first;u32 in_group=remaining<e.per_group?(u32)remaining:e.per_group;
        u32 fb=0,fi=0;for(u32 i=0;i<in_group;i++)if(!bit(e.bmap,group_first+i))fb++;
        for(u32 i=0;i<e.inodes_per_group&&(u64)g*e.inodes_per_group+i<e.inodes;i++)if(!bit(e.imap,(u64)g*e.inodes_per_group+i))fi++;
        if(le16(d+12)!=fb)warn("group %u free blocks %u, bitmap says %u",g,le16(d+12),fb);
        if(le16(d+14)!=fi)warn("group %u free inodes %u, bitmap says %u",g,le16(d+14),fi);
    }
    if(e.orphans)warn("%llu crash orphans parked in the root directory await reclaim at the next mount",(unsigned long long)e.orphans);
    printf("ext2: %llu inodes in use, %llu directories, %llu free blocks, %llu lost blocks, %llu unreferenced inodes\n",(unsigned long long)in_use,(unsigned long long)e.dirs,(unsigned long long)free_blocks,(unsigned long long)lost_blocks,(unsigned long long)unreferenced);
    free(scratch);free(table_data);free(e.gdt);free(e.bmap);free(e.imap);free(e.used);free(e.seen);free(e.links);return 0;
}

/* -------------------------------------------------------------- FAT32 --- */
typedef struct {int fd;u64 base;u32 spc,reserved,fats,fat_sectors,root,clusters,data_start;u32 *fat;u8 *seen;u64 orphans,files,dirs;} Fat;
static u32 fat_next(Fat *f,u32 c){return f->fat[c]&0x0fffffff;}
static int fat_valid(Fat *f,u32 c){return c>=2&&c<f->clusters+2;}
static u64 fat_cluster_offset(Fat *f,u32 c){return f->base+((u64)f->data_start+(u64)(c-2)*f->spc)*512;}
static u32 fat_chain(Fat *f,u32 first,const char *name,u64 size,int is_dir){
    u32 count=0,c=first;
    while(fat_valid(f,c)){if(bit(f->seen,c)){fail("'%s': cluster %u is cross-linked",name,c);return count;}setbit(f->seen,c);count++;c=fat_next(f,c);if(count>f->clusters){fail("'%s': cluster chain loops",name);return count;}}
    if(c<0x0ffffff8&&!(c==0&&first==0)){fail("'%s': chain ends in invalid cluster %u",name,c);}
    if(!is_dir){u64 need=(size+(u64)f->spc*512-1)/((u64)f->spc*512);if(need!=count)fail("'%s': size %llu needs %llu clusters, chain has %u",name,(unsigned long long)size,(unsigned long long)need,count);}
    return count;
}
static void fat_dir(Fat *f,u32 first,int depth,const char *path){
    if(depth>64){fail("%s: directory nesting too deep",path);return;}f->dirs++;
    u32 bytes=f->spc*512;u8 *data=malloc(bytes);char lfn[300];memset(lfn,0,sizeof(lfn));
    for(u32 c=first;fat_valid(f,c);c=fat_next(f,c)){
        if(!readat(f->fd,fat_cluster_offset(f,c),data,bytes)){fail("%s: cluster %u unreadable",path,c);break;}
        for(u32 o=0;o<bytes;o+=32){u8 *e=data+o;if(!e[0]){free(data);return;}if(e[0]==0xe5)continue;
            if((e[11]&0x3f)==0x0f){ /* long-name fragment: 13 UCS-2 characters at fixed offsets, zero/0xffff padded */
                int seq=(e[0]&0x1f)-1;if(seq<0||seq>19)continue;static const int pos[13]={1,3,5,7,9,14,16,18,20,22,24,28,30};
                for(int i=0;i<13;i++){u16 ch=le16(e+pos[i]);if(ch&&ch!=0xffff)lfn[seq*13+i]=ch<128?(char)ch:'?';}
                continue;}
            char name[300];if(lfn[0])strcpy(name,lfn);else{int n=0;for(int i=0;i<8&&e[i]!=' ';i++)name[n++]=e[i];if(e[8]!=' '){name[n++]='.';for(int i=8;i<11&&e[i]!=' ';i++)name[n++]=e[i];}name[n]=0;}
            memset(lfn,0,sizeof(lfn));
            if(e[11]&0x08)continue; /* volume label */
            if(!strcmp(name,".")||!strcmp(name,".."))continue;
            u32 start=le16(e+26)|((u32)le16(e+20)<<16);u32 size=le32(e+28);int is_dir=(e[11]&0x10)!=0;
            char full[600];snprintf(full,sizeof(full),"%s/%s",path,name);
            if(first==f->root&&!strncmp(name,".aurora-orphan-",15)){f->orphans++;if(verbose)printf("  orphan pending reclaim: %s\n",full);}
            if(start==0&&(size||is_dir)){fail("%s: no first cluster",full);continue;}
            if(start&&!fat_valid(f,start)){fail("%s: first cluster %u invalid",full,start);continue;}
            if(start)fat_chain(f,start,full,size,is_dir);
            if(is_dir)fat_dir(f,start,depth+1,full);else f->files++;
        }
    }
    free(data);
}
static int check_fat(int fd,u64 base,u64 sectors){
    Fat f={0};f.fd=fd;f.base=base;u8 bs[512];if(!readat(fd,base,bs,512)){fail("FAT boot sector unreadable");return 4;}
    if(le16(bs+11)!=512||bs[13]==0||le16(bs+510)!=0xaa55){fail("FAT boot sector geometry unsupported");return 4;}
    f.spc=bs[13];f.reserved=le16(bs+14);f.fats=bs[16];f.fat_sectors=le32(bs+36);f.root=le32(bs+44);u32 total=le32(bs+32);u16 fsinfo=le16(bs+48);
    if(!f.fats||!f.fat_sectors||total>sectors){fail("FAT geometry inconsistent with partition");return 4;}
    f.data_start=f.reserved+f.fats*f.fat_sectors;f.clusters=(total-f.data_start)/f.spc;
    printf("FAT32: %u clusters of %u bytes, %u FATs\n",f.clusters,f.spc*512,f.fats);
    size_t fat_bytes=(size_t)f.fat_sectors*512;f.fat=malloc(fat_bytes);if(!readat(fd,base+(u64)f.reserved*512,f.fat,fat_bytes)){fail("FAT unreadable");return 4;}
    if(f.fats>1){u32 *second=malloc(fat_bytes);if(readat(fd,base+((u64)f.reserved+f.fat_sectors)*512,second,fat_bytes)){u32 differ=0;for(u32 c=0;c<f.clusters+2;c++)if((f.fat[c]^second[c])&0x0fffffff)differ++;if(differ)warn("%u FAT entries differ between the two copies",differ);}free(second);}
    if((f.fat[0]&0x0fffffff)<0x0ffffff0||(f.fat[1]&0x0fffffff)<0x0ffffff8)warn("FAT media/end-of-chain markers unusual");
    if(!(f.fat[1]&0x08000000))warn("FAT dirty flag set: the volume was not cleanly unmounted");
    f.seen=calloc(1,(f.clusters+2+7)/8);
    if(!fat_valid(&f,f.root)){fail("root cluster %u invalid",f.root);return 4;}
    fat_chain(&f,f.root,"/",0,1);fat_dir(&f,f.root,0,"");
    u32 lost=0,free_clusters=0;for(u32 c=2;c<f.clusters+2;c++){u32 v=fat_next(&f,c);if(!v)free_clusters++;else if(!bit(f.seen,c)&&v!=0x0ffffff7)lost++;}
    if(lost)warn("%u clusters allocated but unreferenced (lost clusters)",lost);
    if(fsinfo&&fsinfo<f.reserved){u8 info[512];if(readat(fd,base+(u64)fsinfo*512,info,512)&&le32(info)==0x41615252){u32 reported=le32(info+488);if(reported!=0xffffffff&&reported!=free_clusters)warn("FSInfo free count %u, FAT says %u",reported,free_clusters);}}
    if(f.orphans)warn("%llu crash orphans parked in the exchange root await reclaim at the next mount",(unsigned long long)f.orphans);
    printf("FAT32: %llu files, %llu directories, %u free clusters, %u lost clusters\n",(unsigned long long)f.files,(unsigned long long)f.dirs,free_clusters,lost);
    free(f.fat);free(f.seen);return 0;
}

/* ----------------------------------------------------------- AuroraFS --- */
static int check_aurorafs(int fd){
    u8 sector[512];if(!readat(fd,512*512,sector,512)){fail("AuroraFS superblock unreadable");return 4;}
    if(memcmp(sector,"AURFS01",8)){fail("AuroraFS magic missing");return 4;}
    u8 dir[2048];if(!readat(fd,513*512,dir,2048)){fail("AuroraFS directory unreadable");return 4;}
    int used=0;
    for(int i=0;i<32;i++){u8 *e=dir+i*64;u32 size=le32(e+32),flag=le32(e+36);
        if(flag>1){fail("AuroraFS slot %d has used flag %u",i,flag);continue;}if(!flag)continue;used++;
        if(size>65536)fail("AuroraFS slot %d size %u exceeds 64 KiB",i,size);
        int valid=e[0]!=0,length=0;for(;length<32&&e[length];length++){char c=e[length];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))valid=0;}
        if(length==32)valid=0;if(!valid)fail("AuroraFS slot %d has an invalid name",i);
        for(int j=0;j<i;j++)if(le32(dir+j*64+36)==1&&!strncmp((char *)dir+j*64,(char *)e,32))fail("AuroraFS slots %d and %d share a name",j,i);
    }
    printf("AuroraFS: %d of 32 slots in use\n",used);return 0;
}
int main(int argc,char **argv){
    const char *disk="/dev/disk",*boot="/dev/boot";int do_disk=1,do_boot=1;
    for(int i=1;i<argc;i++){if(!strcmp(argv[i],"-v"))verbose=1;else if(!strcmp(argv[i],"--no-boot"))do_boot=0;else if(!strcmp(argv[i],"--no-disk"))do_disk=0;else if(!strcmp(argv[i],"--disk")&&i+1<argc)disk=argv[++i];else if(!strcmp(argv[i],"--boot")&&i+1<argc)boot=argv[++i];else{fprintf(stderr,"usage: fsck-aurora [-v] [--no-boot] [--no-disk] [--disk DEV] [--boot DEV]\n");return 2;}}
    if(do_disk){int fd=open(disk,O_RDONLY);if(fd<0){printf("%s: %s\n",disk,strerror(errno));return 8;}
        struct stat st;fstat(fd,&st);u64 sectors=(u64)st.st_size/512;printf("Checking %s (%llu sectors)\n",disk,(unsigned long long)sectors);
        Partition parts[8];int count=check_gpt(fd,sectors,parts);
        for(int i=0;i<count;i++){if(parts[i].kind==1)check_ext2(fd,parts[i].start*512,parts[i].end-parts[i].start+1);else if(parts[i].kind==2)check_fat(fd,parts[i].start*512,parts[i].end-parts[i].start+1);}
        close(fd);}
    if(do_boot){int fd=open(boot,O_RDONLY);if(fd<0)printf("%s: %s (skipped)\n",boot,strerror(errno));else{printf("Checking %s\n",boot);check_aurorafs(fd);close(fd);}}
    if(errors){printf("FSCK: %d errors, %d warnings\n",errors,warnings);return 4;}
    if(warnings){printf("FSCK: %d warnings\n",warnings);return 1;}
    printf("FSCK: clean\n");return 0;
}
