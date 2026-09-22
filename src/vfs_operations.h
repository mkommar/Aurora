/* Namespace operations preserve open file descriptions when names disappear:
 * ext2/FAT names are parked in a protected, marked recovery directory and
 * removed on last close. Legacy root lookalikes are never reclaimed. */
static u64 vfs_temporary_counter;
static int vfs_recovery_ready[2];
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
    int error=ext2_dirty();if(error)return -error;
    return is_dir?-ext4_dir_mv(from,to):-ext4_frename(from,to);
}
static int vfs_remove(const char *path,int is_dir){
    if(aurorafs_path(path)){int slot=file_find(aurorafs_name(path));if(slot<0)return -2;if(is_dir)return -20;memset(&directory[slot],0,sizeof(FileEntry));return file_write_directory(slot)?0:-5;}
    if(fat_ready&&fat_path(path)){FRESULT error=f_unlink(fat_name(path));return error?fat_error(error):0;}
    int error=ext2_dirty();if(error)return -error;
    return is_dir?-ext4_dir_rm(path):-ext4_fremove(path);
}
/* A short 8.3 directory avoids alternate FAT aliases. A versioned marker
 * distinguishes directories created by this kernel from preexisting content.
 * A collision or incomplete initialization disables parking, never adopts it. */
static const char *vfs_recovery_root(int fat){return fat?"/exchange/AURORARC":"/AURORARC";}
static int vfs_recovery_init(int fat){
    const char *root=vfs_recovery_root(fat),*magic="Aurora recovery directory v1\n";
    char marker[64],data[40];ns_copy(marker,root);ns_copy(marker+ns_length(marker),"/OWNER");
    int kind=vfs_kind(root),create=kind==-2;if(kind!=2&&!create)return 0;
    u64 length=ns_length(magic);memset(data,0,sizeof(data));
    if(fat){
        if(create&&f_mkdir(fat_name(root))!=FR_OK)return 0;
        FIL file;FRESULT error=f_open(&file,fat_name(marker),create?FA_WRITE|FA_CREATE_NEW:FA_READ);if(error)return 0;
        UINT done=0;if(create)error=f_write(&file,magic,length,&done);
        else if(f_size(&file)!=length)error=FR_INVALID_OBJECT;
        else error=f_read(&file,data,length,&done);
        FRESULT closed=f_close(&file);if(error||closed||done!=length)return 0;
    }else{
        if(create&&ext4_dir_mk(root))return 0;
        if(!create){uint32_t mode;if(ext4_mode_get(marker,&mode)||(mode&0170000)!=0100000)return 0;}
        ext4_file file;int error=ext4_fopen(&file,marker,create?"w":"r");if(error)return 0;
        size_t done=0;if(create)error=ext4_fwrite(&file,magic,length,&done);
        else if(ext4_fsize(&file)!=length)error=5;
        else error=ext4_fread(&file,data,length,&done);
        int closed=ext4_fclose(&file);if(error||closed||done!=length)return 0;
        if(create&&ext4_cache_flush("/"))return 0;
    }
    if(!create)for(u64 i=0;i<length;i++)if(data[i]!=magic[i])return 0;
    return native_disk_flush();
}
static const char *vfs_root(const char *path){return aurorafs_path(path)?"/aurorafs":vfs_recovery_root(fat_ready&&fat_path(path));}
static int vfs_orphan_name(const char *name,u32 length){
    const char *prefix=".aurora-orphan-";if(length!=31)return 0;
    for(int i=0;i<15;i++)if(name[i]!=prefix[i])return 0;
    for(int i=15;i<31;i++)if(!((name[i]>='0'&&name[i]<='9')||(name[i]>='a'&&name[i]<='f')))return 0;
    return 1;
}
static int vfs_temporary(char *path,const char *root){
    if(!aurorafs_path(root)&&!vfs_recovery_ready[fat_ready&&fat_path(root)])return -5;
    for(int attempt=0;attempt<4096;attempt++){
        ns_copy(path,root);u64 n=ns_length(path);ns_copy(path+n,"/.aurora-orphan-");n+=16;u64 value=++vfs_temporary_counter;
        for(int i=15;i>=0;i--)path[n+15-i]="0123456789abcdef"[(value>>(i*4))&15];path[n+16]=0;
        int kind=vfs_kind(path);if(kind==-2)return 0;if(kind<0)return kind;
    }
    return -28;
}
/* Only empty parked directories are disposable; never recursively reclaim. */
static int vfs_empty_directory(const char *path){
    if(fat_ready&&fat_path(path)){DIR dir;FILINFO info;if(f_opendir(&dir,fat_name(path))!=FR_OK)return 0;
        FRESULT error=f_readdir(&dir,&info);f_closedir(&dir);return error==FR_OK&&!info.fname[0];}
    ext4_dir dir;if(ext4_dir_open(&dir,path))return 0;const ext4_direntry *entry;int empty=1;
    while((entry=ext4_dir_entry_next(&dir)))if(!(entry->name_length==1&&entry->name[0]=='.')&&!(entry->name_length==2&&entry->name[0]=='.'&&entry->name[1]=='.')){empty=0;break;}
    ext4_dir_close(&dir);return empty;
}
static int vfs_reclaim_one(const char *root){
    char path[256];u32 length=0;int is_dir=0;
    if(fat_ready&&fat_path(root)){DIR dir;FILINFO info;if(f_opendir(&dir,fat_name(root))!=FR_OK)return 0;
        while(f_readdir(&dir,&info)==FR_OK&&info.fname[0]){u32 n=(u32)ns_length(info.fname);if(vfs_orphan_name(info.fname,n)){length=n;is_dir=(info.fattrib&AM_DIR)!=0;ns_copy(path,root);ns_copy(path+ns_length(root),"/");ns_copy(path+ns_length(root)+1,info.fname);break;}}
        f_closedir(&dir);
    }else if(ext2_ready){ext4_dir dir;if(ext4_dir_open(&dir,root))return 0;const ext4_direntry *entry;
        while((entry=ext4_dir_entry_next(&dir)))if(vfs_orphan_name((const char *)entry->name,entry->name_length)){length=entry->name_length;is_dir=entry->inode_type==2;u64 n=ns_length(root);ns_copy(path,root);path[n++]='/';memcpy(path+n,entry->name,length);path[n+length]=0;break;}
        ext4_dir_close(&dir);
    }
    if(!length)return 0;
    if(is_dir&&!vfs_empty_directory(path)){serial("VFS: nonempty recovery directory preserved\r\n");return 0;}
    int error=vfs_remove(path,is_dir);if(error){serial("VFS: orphan removal failed error=");hex(-error);serial("\r\n");return 0;}
    vfs_orphans_reclaimed++;return 1;
}
static void vfs_reclaim_orphans(void){
    for(int fat=0;fat<2;fat++){
        if(fat?!fat_ready:!ext2_ready)continue;
        vfs_recovery_ready[fat]=vfs_recovery_init(fat);
        if(!vfs_recovery_ready[fat]){serial("VFS: recovery directory unavailable; parking disabled\r\n");continue;}
        for(int i=0;i<4096&&vfs_reclaim_one(vfs_recovery_root(fat));i++){}
    }
    if(vfs_orphans_reclaimed){serial("VFS: reclaimed crash orphans count=");hex(vfs_orphans_reclaimed);serial("\r\n");}
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
static int vfs_close_deleted(int index){
    if(*(u32 *)(NFILES[index].pad+8)&&!vfs_open_index(index)){
        int error=vfs_remove(NFILES[index].path,NFILES[index].kind==2);if(error)return error;NFILES[index].kind=0;
    }return 0;
}
