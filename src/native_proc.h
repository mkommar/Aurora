/* Read-only process filesystem. Bytes and directory records come from the
 * live process table rather than from a userspace directory. */
#define PROC_MARKER 0x505243U
#define PROC_DIR 2
#define PROC_FILE 1
#define PROC_LINK 3
static int proc_path(const char *p){return p[0]=='/'&&p[1]=='p'&&p[2]=='r'&&p[3]=='o'&&p[4]=='c'&&(!p[5]||p[5]=='/');}
static int proc_is_entry(int i){return i>=0&&(u32)i<native_count&&*(u32 *)(NFILES[i].pad+20)==PROC_MARKER;}
static int proc_id(u32 pid){for(int i=APP_FIRST;i<TASK_COUNT;i++)if(native_active[i]&&tasks[i].state!=DEAD&&native_process[i].tgid==pid)return i;return -1;}
static int proc_number(const char *s,u32 *out){u32 n=0;if(!*s)return 0;while(*s&&*s!='/'){if(*s<'0'||*s>'9'||n>429496729U)return 0;n=n*10+*s++-'0';}*out=n;return 1;}
static int proc_pid_path(const char *p,u32 *pid,const char **rest){char name[16];int n=0;const char *s=p+6;if(!*s)return 0;while(*s&&*s!='/'&&n<15)name[n++]=*s++;name[n]=0;if(ns_equal(name,"self"))*pid=native_process[current_task].tgid;else if(!proc_number(name,pid))return 0;*rest=s;return proc_id(*pid)>=0;}
static int proc_kind(const char *p){
    if(!proc_path(p))return -1;if(!p[5]||ns_equal(p,"/proc/self"))return PROC_DIR;
    if(ns_equal(p,"/proc/meminfo")||ns_equal(p,"/proc/uptime")||ns_equal(p,"/proc/mounts"))return PROC_FILE;
    u32 pid;const char *r;if(!proc_pid_path(p,&pid,&r))return -2;if(!*r)return PROC_DIR;
    if(ns_equal(r,"/status")||ns_equal(r,"/cmdline"))return PROC_FILE;if(ns_equal(r,"/fd"))return PROC_DIR;
    if(r[0]!='/'||r[1]!='f'||r[2]!='d'||r[3]!='/')return -2;u32 fd;if(!proc_number(r+4,&fd)||fd>=NATIVE_FDS)return -2;
    return native_process[proc_id(pid)].fd[fd].kind?PROC_LINK:-2;
}
static int proc_find(const char *p){int kind=proc_kind(p);if(kind<0)return proc_path(p)?kind:-1;for(u32 i=0;i<native_count;i++)if(proc_is_entry(i)&&ns_equal(NFILES[i].path,p)){NFILES[i].kind=kind;return i;}
    u32 i;for(i=0;i<native_count;i++)if(!NFILES[i].kind)break;if(i==NATIVE_FILE_CACHE)return -28;if(i==native_count)native_count++;
    NativeFile *f=&NFILES[i];memset(f,0,sizeof(*f));ns_copy(f->path,p);f->kind=kind;*(u32 *)f->pad=kind==PROC_DIR?0755:kind==PROC_LINK?0777:0444;*(u32 *)(f->pad+4)=1;*(u32 *)(f->pad+20)=PROC_MARKER;return (int)i;}
static void proc_add(char *out,u64 *used,const char *s){u64 n=ns_length(s);if(*used+n<4096){memcpy(out+*used,s,n);*used+=n;}}
static void proc_num(char *out,u64 *used,const char *prefix,u64 value,const char *suffix){char d[24];int n=0;proc_add(out,used,prefix);if(!value)d[n++]='0';while(value){d[n++]=(char)('0'+value%10);value/=10;}while(n)out[(*used)++]=d[--n];proc_add(out,used,suffix);}
static u64 proc_data(int i,char *out){const char *p=NFILES[i].path;u64 n=0;
    if(ns_equal(p,"/proc/uptime")){proc_num(out,&n,"",timer_ticks/100,".");out[n++]=(char)('0'+(timer_ticks%100)/10);proc_add(out,&n,"  ");proc_num(out,&n,"",timer_ticks/100,".");out[n++]=(char)('0'+(timer_ticks%100)/10);proc_add(out,&n,"\n");}
    else if(ns_equal(p,"/proc/meminfo")){proc_num(out,&n,"MemFree:        ",native_free_pages*4," kB\n");proc_num(out,&n,"MemAvailable:   ",native_free_pages*4," kB\n");}
    else if(ns_equal(p,"/proc/mounts")){if(ext2_ready)proc_add(out,&n,"/dev/disk / ext2 rw 0 0\n");if(fat_ready)proc_add(out,&n,"/dev/disk /exchange fat rw 0 0\n");if(fs_ready)proc_add(out,&n,"aurorafs /aurorafs aurorafs rw 0 0\n");}
    else {u32 pid;const char *r;if(proc_pid_path(p,&pid,&r)){int id=proc_id(pid);if(id>=0&&ns_equal(r,"/cmdline")){proc_add(out,&n,native_process[id].exe);out[n++]=0;}else if(id>=0&&ns_equal(r,"/status")){NativeProcess *q=&native_process[id];proc_add(out,&n,"Name:\t");proc_add(out,&n,q->name);proc_add(out,&n,"\nState:\t");proc_add(out,&n,tasks[id].state==STOPPED?"T (stopped)":"R (running)");proc_add(out,&n,"\n");proc_num(out,&n,"Tgid:\t",q->tgid,"\n");proc_num(out,&n,"Pid:\t",id+100,"\n");proc_num(out,&n,"PPid:\t",q->parent>=0?q->parent+100:1,"\n");proc_add(out,&n,"Uid:\t1000\t1000\t1000\t1000\nGid:\t1000\t1000\t1000\t1000\n");}}
    }return n;}
static i64 proc_read(int i,u64 off,void *buf,u64 count){char data[4096];u64 n=proc_data(i,data);if(off>=n)return 0;if(count>n-off)count=n-off;memcpy(buf,data+off,count);return count;}
static void proc_dirent(u8 *out,u64 *done,u64 size,u64 ino,const char *name,u8 type){u64 n=ns_length(name),len=(20+n+7)&~7ULL;if(len>size-*done)return;memset(out+*done,0,len);*(u64 *)(out+*done)=ino;*(u64 *)(out+*done+8)=ino;*(u16 *)(out+*done+16)=len;out[*done+18]=type;memcpy(out+*done+19,name,n);*done+=len;}
static i64 proc_getdents(int i,u64 *off,void *buf,u64 size){u8 *out=buf;u64 done=0,pos=*off;const char *p=NFILES[i].path;if(pos==0){proc_dirent(out,&done,size,1,".",4);proc_dirent(out,&done,size,2,"..",4);}
    if(ns_equal(p,"/proc")){if(pos==0)proc_dirent(out,&done,size,1,"self",4);for(int id=APP_FIRST;id<TASK_COUNT;id++)if(native_active[id]&&tasks[id].state!=DEAD){char name[16];u32 v=id+100;int n=0;while(v){name[n++]=(char)('0'+v%10);v/=10;}for(int j=0;j<n/2;j++){char c=name[j];name[j]=name[n-1-j];name[n-1-j]=c;}name[n]=0;if((u64)(id-APP_FIRST+1)>=pos)proc_dirent(out,&done,size,id+100,name,4);}}
    else {u32 pid;const char *r;proc_pid_path(p,&pid,&r);if(!*r&&pos==0){proc_dirent(out,&done,size,1,"status",8);proc_dirent(out,&done,size,2,"cmdline",8);proc_dirent(out,&done,size,3,"fd",4);}else if(ns_equal(r,"/fd")){int id=proc_id(pid),seen=0;if(id>=0)for(int fd=0;fd<NATIVE_FDS;fd++)if(native_process[id].fd[fd].kind){if((u64)seen++>=pos){char name[4];name[0]=(char)('0'+fd/10);name[1]=(char)('0'+fd%10);name[2]=0;proc_dirent(out,&done,size,fd+1,name,10);}}}}
    *off=pos+done;return done;}
static i64 proc_stat(int i,u8 *out){NativeFile *f=&NFILES[i];memset(out,0,144);*(u64 *)out=0x900000+i;*(u64 *)(out+8)=i+1;*(u64 *)(out+16)=1;*(u32 *)(out+24)=(f->kind==PROC_DIR?0040000:f->kind==PROC_LINK?0120000:0100000)|(*(u32 *)f->pad&0777);*(u32 *)(out+28)=1000;*(u32 *)(out+32)=1000;*(u64 *)(out+56)=4096;if(f->kind==PROC_FILE){char data[4096];*(u64 *)(out+48)=proc_data(i,data);}return 0;}
static i64 proc_readlink(int i,void *buf,u64 count){NativeFile *f=&NFILES[i];u32 pid,fd=0;const char *r;proc_pid_path(f->path,&pid,&r);for(const char *s=r+4;*s;s++)fd=fd*10+*s-'0';int id=proc_id(pid);char target[256]="";NativeFd *q=&native_process[id].fd[fd];if(q->kind==4)ns_copy(target,"/dev/tty");else if(q->kind==1)ns_copy(target,NFILES[q->index].path);else if(q->kind==5)ns_copy(target,"/dev/null");else if(q->kind==9)ns_copy(target,q->index?"/dev/boot":"/dev/disk");else if(q->kind==2||q->kind==3){u64 n=0;proc_num(target,&n,q->kind==6?"socket:[":"pipe:[",q->index,"]");}else if(q->kind==6){u64 n=0;proc_num(target,&n,"socket:[",q->index,"]");}u64 n=ns_length(target);if(n>count)n=count;memcpy(buf,target,n);return n;}
