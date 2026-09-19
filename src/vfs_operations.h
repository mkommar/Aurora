/* Namespace operations preserve open file descriptions when names disappear:
 * the name is parked as a hidden orphan in its volume root and removed on the
 * last close. A crash leaves the parked names behind; mount reclaims them. */
static u64 vfs_temporary_counter;
volatile u64 vfs_orphans_reclaimed;
static int vfs_open_index(int index){
    for(u32 id=APP_FIRST;id<TASK_COUNT;id++)if(native_process[id].fd&&tasks[id].state!=DEAD)for(int fd=0;fd<NATIVE_FDS;fd++)if(native_process[id].fd[fd].kind==1&&native_process[id].fd[fd].index==index)return 1;
    return 0;
}
static int vfs_kind(const char *path){
    if(aurorafs_path(path)){const char *name=aurorafs_name(path);return !*name?2:name_valid(name)&&file_find(name)>=0?1:-2;}
    if(fat_ready&&fat_path(path)){FILINFO info;FRESULT error=f_stat(fat_name(path),&info);return error?fat_error(error):(info.fattrib&AM_DIR)?2:1;}
    if(!ext2_ready)return -2;
    uint32_t mode;int error=ext4_mode_get(path,&mode);return error?-error:(mode&0170000)==0040000?2:(mode&0170000)==0120000?3:1;
}
static int vfs_move(const char *from,const char *to,int is_dir){
    if(aurorafs_path(from))return is_dir?-1:aurorafs_rename(from,to);
    if(fat_ready&&fat_path(from)){FRESULT error=f_rename(fat_name(from),fat_name(to));return error?fat_error(error):0;}
    return is_dir?-ext4_dir_mv(from,to):-ext4_frename(from,to);
}
static int vfs_remove(const char *path,int is_dir){
    if(aurorafs_path(path)){int slot=file_find(aurorafs_name(path));if(slot<0)return -2;if(is_dir)return -20;memset(&directory[slot],0,sizeof(FileEntry));return file_write_directory(slot)?0:-5;}
    if(fat_ready&&fat_path(path)){FRESULT error=f_unlink(fat_name(path));return error?fat_error(error):0;}
    return is_dir?-ext4_dir_rm(path):-ext4_fremove(path);
}
/* Volume root of `path`, where its orphans are parked. */
static const char *vfs_root(const char *path){return aurorafs_path(path)?"/aurorafs":fat_ready&&fat_path(path)?"/exchange":"";}
static int vfs_orphan_name(const char *name,u32 length){const char *prefix=".aurora-orphan-";if(length!=31)return 0;for(int i=0;i<15;i++)if(name[i]!=prefix[i])return 0;return 1;}
static int vfs_temporary(char *path,const char *root){
    for(int attempt=0;attempt<4096;attempt++){
        ns_copy(path,root);u64 n=ns_length(path);ns_copy(path+n,"/.aurora-orphan-");n+=16;u64 value=++vfs_temporary_counter;
        for(int i=15;i>=0;i--)path[n+15-i]="0123456789abcdef"[(value>>(i*4))&15];path[n+16]=0;
        if(vfs_kind(path)==-2)return 0;
    }
    return -28;
}
/* Remove one parked orphan under `root`; 1 when something was removed. */
static int vfs_reclaim_one(const char *root){
    char path[256];u32 length=0;int is_dir=0;
    if(fat_ready&&fat_path(root)){DIR dir;FILINFO info;if(f_opendir(&dir,fat_name(root))!=FR_OK)return 0;
        while(f_readdir(&dir,&info)==FR_OK&&info.fname[0]){u32 n=(u32)ns_length(info.fname);if(vfs_orphan_name(info.fname,n)){length=n;is_dir=(info.fattrib&AM_DIR)!=0;ns_copy(path,root);ns_copy(path+ns_length(root),"/");ns_copy(path+ns_length(root)+1,info.fname);break;}}
        f_closedir(&dir);
    }else if(ext2_ready){ext4_dir dir;if(ext4_dir_open(&dir,"/"))return 0;const ext4_direntry *entry;
        while((entry=ext4_dir_entry_next(&dir)))if(vfs_orphan_name((const char *)entry->name,entry->name_length)){length=entry->name_length;is_dir=entry->inode_type==2;path[0]='/';memcpy(path+1,entry->name,length);path[1+length]=0;break;}
        ext4_dir_close(&dir);
    }
    if(!length)return 0;
    int error=vfs_remove(path,is_dir);if(error){serial("VFS: orphan removal failed error=");hex(-error);serial("\r\n");return 0;}
    vfs_orphans_reclaimed++;return 1;
}
static void vfs_reclaim_orphans(void){
    u64 before=vfs_orphans_reclaimed;
    for(int i=0;i<4096&&vfs_reclaim_one("");i++){}
    for(int i=0;i<4096&&vfs_reclaim_one("/exchange");i++){}
    if(vfs_orphans_reclaimed!=before){serial("VFS: reclaimed crash orphans count=");hex(vfs_orphans_reclaimed-before);serial("\r\n");}
}
static void vfs_cache_move(const char *from,const char *to){
    u64 length=ns_length(from),target=ns_length(to);
    for(u32 i=0;i<native_count;i++)if(NFILES[i].kind){char *path=NFILES[i].path;u64 n=0;while(n<length&&path[n]==from[n])n++;
        if(n==length&&(!path[n]||path[n]=='/')){char copy[256];u64 suffix=ns_length(path+n);
            if(target+suffix<256){memcpy(copy,to,target);memcpy(copy+target,path+n,suffix+1);ns_copy(path,copy);}
        }
    }
}
static int vfs_unlink(const char *path){
    int kind=vfs_kind(path);if(kind<0)return kind;if(kind==2)return -21;
    int index=-1;for(u32 i=0;i<native_count;i++)if(NFILES[i].kind&&ns_equal(NFILES[i].path,path)){index=i;break;}
    if(index>=0&&vfs_open_index(index)){
        char temporary[256];int error=vfs_temporary(temporary,vfs_root(path));if(error)return error;
        error=vfs_move(path,temporary,0);if(error)return error;ns_copy(NFILES[index].path,temporary);*(u32 *)(NFILES[index].pad+8)=1;return 0;
    }
    int error=vfs_remove(path,0);if(!error&&index>=0)NFILES[index].kind=0;return error;
}
static int vfs_rename(const char *from,const char *to,u32 flags){
    if(flags&~1U)return -95;
    if((fat_ready&&fat_path(from))!=(fat_ready&&fat_path(to))||aurorafs_path(from)!=aurorafs_path(to))return -18;
    if(ns_equal(from,to))return 0;
    int kind=vfs_kind(from),target=vfs_kind(to);if(kind<0)return kind;if(target<0&&target!=-2)return target;
    if(target>=0&&(flags&1))return -17;
    if(target==2&&kind!=2)return -21;if(target>0&&target!=2&&kind==2)return -20;
    if(target==2){
        if(fat_ready&&fat_path(to)){DIR dir;FILINFO info;FRESULT error=f_opendir(&dir,fat_name(to));if(error)return fat_error(error);f_readdir(&dir,&info);f_closedir(&dir);if(info.fname[0])return -39;}
        else if(aurorafs_path(to))return -16;
        else{ext4_dir dir;int error=ext4_dir_open(&dir,to);if(error)return -error;const ext4_direntry *entry;int nonempty=0;
            while((entry=ext4_dir_entry_next(&dir)))if(!(entry->name_length==1&&entry->name[0]=='.')&&!(entry->name_length==2&&entry->name[0]=='.'&&entry->name[1]=='.'))nonempty=1;
            ext4_dir_close(&dir);if(nonempty)return -39;}
    }
    char temporary[256];int old=-1;
    if(target>0){int error=vfs_temporary(temporary,vfs_root(to));if(error)return error;
        error=vfs_move(to,temporary,target==2);if(error)return error;
        for(u32 i=0;i<native_count;i++)if(NFILES[i].kind&&ns_equal(NFILES[i].path,to)){old=i;break;}
    }
    int error=vfs_move(from,to,kind==2);
    if(error){if(target>0)vfs_move(temporary,to,target==2);return error;}
    if(target>0){if(old>=0&&vfs_open_index(old)){ns_copy(NFILES[old].path,temporary);*(u32 *)(NFILES[old].pad+8)=1;}
        else{vfs_remove(temporary,target==2);if(old>=0)NFILES[old].kind=0;}}
    vfs_cache_move(from,to);return 0;
}
static void vfs_close_deleted(int index){
    if(*(u32 *)(NFILES[index].pad+8)&&!vfs_open_index(index)){vfs_remove(NFILES[index].path,NFILES[index].kind==2);NFILES[index].kind=0;}
}
