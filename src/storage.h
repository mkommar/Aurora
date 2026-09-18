/* AuroraFS over the primary ATA channel. Before scheduling starts, PIO polls
 * with bounded spins. Afterwards IRQ14 completes transfers: reads and flushes
 * sleep until the drive interrupts, writes poll DRQ (no interrupt precedes the
 * data phase) and then sleep for the completion interrupt. A sequence number
 * tags each command so a late interrupt from an earlier command cannot
 * complete a newer one, and a tick deadline bounds every sleep. Callers hold
 * the filesystem mutex, which serializes the channel. */
static FileEntry directory[FS_FILES];
static u8 disk_sector[512];
#define executable ((u8 *)(KERNEL_STATE+0x60000))
static int fs_ready;
static int ata_irq_mode,ata_waiter=-1;
static u32 ata_sequence,ata_completed_sequence;
static u64 ata_deadline;
volatile u64 ata_interrupts,ata_suspensions,ata_timeouts,ata_stale_interrupts,ata_polled;
static int ata_wait(int data) {
    for(u32 i=0;i<1000000;i++) {
        u8 s=inb(0x1f7);
        if(s==0||s==255)return 0;
        if(!(s&128)) { if(s&0x21)return 0;if(!data||(s&8))return 1; }
    }
    return 0;
}
static void ata_interrupt(u32 irq){
    if(irq!=14)return;(void)inb(0x1f7);ata_interrupts++;
    if(ata_waiter<0){ata_stale_interrupts++;return;}
    ata_completed_sequence=ata_sequence;tasks[ata_waiter].state=RUNNABLE;ata_waiter=-1;
}
static void ata_timeout(void){
    if(ata_waiter>=0&&timer_ticks>=ata_deadline){ata_timeouts++;tasks[ata_waiter].state=RUNNABLE;ata_waiter=-1;}
}
/* Sleep for the interrupt of command `sequence`. Returns 1 when it arrived. */
static int ata_irq_ready(void){return kernel_started&&ata_irq_mode&&current_task<TASK_COUNT;}
/* Arm the waiter before issuing a command: another CPU may take IRQ14 before
 * this one sleeps, and the handler must recognise the completion. */
static void ata_arm(void){if(ata_irq_ready())ata_waiter=current_task;}
static int ata_sleep(u32 sequence,u64 ticks){
    if(!ata_irq_ready()){ata_polled++;return -1;}
    if(ata_completed_sequence==sequence){ata_waiter=-1;return 1;}
    ata_waiter=current_task;ata_deadline=timer_ticks+ticks;tasks[current_task].state=WAIT_IO;ata_suspensions++;kernel_suspend();
    return ata_completed_sequence==sequence;
}
static int ata_transfer(u32 lba,void *buffer,int write,int slave) {
    if(lba>=0x10000000)return 0;
    outb(0x1f6,(slave?0xf0:0xe0)|(lba>>24));
    for(int i=0;i<4;i++)(void)inb(0x3f6);
    if(!ata_wait(0))return 0;
    u32 sequence=++ata_sequence;ata_arm();
    outb(0x1f2,1);outb(0x1f3,lba);outb(0x1f4,lba>>8);outb(0x1f5,lba>>16);
    outb(0x1f7,write?0x30:0x20);
    if(!write){int slept=ata_sleep(sequence,300);if(!slept)return 0;}
    if(!ata_wait(1))return 0;
    u64 count=256;
    if(write)__asm__ volatile("rep outsw":"+S"(buffer),"+c"(count):"d"((u16)0x1f0):"memory");
    else __asm__ volatile("rep insw":"+D"(buffer),"+c"(count):"d"((u16)0x1f0):"memory");
    if(write){int slept=ata_sleep(sequence,300);if(!slept)return 0;}
    for(int i=0;i<4;i++)(void)inb(0x3f6);
    return ata_wait(0);
}
static int sector_io(u32 lba,void *buffer,int write) {if(lba>=32768)return 0;return ata_transfer(lba,buffer,write,0);}
static int disk_flush(void) {
    u32 sequence=++ata_sequence;ata_arm();outb(0x1f7,0xe7);
    /* Host-backed large images can take longer to flush than a sector I/O. */
    int slept=ata_sleep(sequence,3000);if(!slept)return 0;
    if(slept>0){u8 s=inb(0x1f7);return s&&s!=255&&!(s&0xa1);}
    for(u32 i=0;i<100000000;i++){u8 s=inb(0x1f7);if(!s||s==255)return 0;if(!(s&128))return !(s&0x21);}
    return 0;
}
static int name_valid(const char *s) {
    if(!s[0])return 0;
    for(int i=0;i<32;i++) {
        char c=s[i];if(!c)return 1;
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))return 0;
    }
    return 0;
}
static int name_equal(const char *a,const char *b) {for(int i=0;i<32;i++){if(a[i]!=b[i])return 0;if(!a[i])return 1;}return 0;}
static void filesystem_init(void) {
    if(!sector_io(FS_LBA,disk_sector,0))return;
    const char magic[8]={'A','U','R','F','S','0','1',0};
    for(int i=0;i<8;i++)if(disk_sector[i]!=(u8)magic[i])return;
    for(int i=0;i<4;i++)if(!sector_io(FS_LBA+1+i,(u8 *)directory+i*512,0))return;
    for(int i=0;i<FS_FILES;i++) {
        if(directory[i].used>1||directory[i].size>FS_MAX_SIZE)return;
        if(directory[i].used) {
            if(!name_valid(directory[i].name))return;
            for(int j=0;j<i;j++)if(directory[j].used&&name_equal(directory[i].name,directory[j].name))return;
        }
    }
    fs_ready=1;serial("FS: AuroraFS mounted read/write\r\n");
}
static int file_find(const char *name) {for(int i=0;i<FS_FILES;i++)if(directory[i].used&&name_equal(name,directory[i].name))return i;return -1;}
static i64 file_read(int index,void *buffer,u64 capacity) {
    u64 size=directory[index].size;if(size>capacity)size=capacity;
    for(u64 offset=0;offset<size;offset+=512) {
        if(!sector_io(FS_DATA_LBA+index*128+offset/512,disk_sector,0))return ERR_IO;
        u64 n=size-offset;if(n>512)n=512;memcpy((u8 *)buffer+offset,disk_sector,n);
    }
    return size;
}
static i64 file_transfer(u64 address,int write) {
    FileRequest *source=user_buffer(current_task,address,sizeof(FileRequest),0);
    if(!source)return ERR_POINTER;
    FileRequest request=*source;
    if(!fs_ready)return ERR_IO;
    if(!name_valid(request.name))return ERR_NAME;
    if(request.size>FS_MAX_SIZE)return ERR_LIMIT;
    void *buffer=request.size?user_buffer(current_task,request.buffer,request.size,!write):0;
    if(request.size&&!buffer)return ERR_POINTER;
    int index=file_find(request.name);
    if(!write)return index<0?ERR_NOT_FOUND:file_read(index,buffer,request.size);
    if(index<0)for(int i=0;i<FS_FILES;i++)if(!directory[i].used){index=i;break;}
    if(index<0)return ERR_LIMIT;
    for(u64 offset=0;offset<request.size;offset+=512) {
        memset(disk_sector,0,512);u64 n=request.size-offset;if(n>512)n=512;
        memcpy(disk_sector,(u8 *)buffer+offset,n);
        if(!sector_io(FS_DATA_LBA+index*128+offset/512,disk_sector,1))return ERR_IO;
    }
    if(!disk_flush())return ERR_IO;
    FileEntry previous=directory[index];
    memset(&directory[index],0,sizeof(FileEntry));memcpy(directory[index].name,request.name,32);
    directory[index].used=1;directory[index].size=request.size;
    if(!sector_io(FS_LBA+1+index/8,(u8 *)directory+(index/8)*512,1)||!disk_flush()) {
        directory[index]=previous;return ERR_IO;
    }
    return request.size;
}
typedef struct {
    u8 ident[16];u16 type,machine;u32 version;u64 entry,phoff,shoff;
    u32 flags;u16 ehsize,phentsize,phnum,shentsize,shnum,shstrndx;
} ElfHeader;
typedef struct {u32 type,flags;u64 offset,vaddr,paddr,filesz,memsz,align;} ElfSegment;
static i64 spawn_application(u64 address) {
    SpawnRequest *source=user_buffer(current_task,address,sizeof(SpawnRequest),0);
    if(!source)return ERR_POINTER;
    SpawnRequest request=*source;
    if(!name_valid(request.name)||request.args[127])return ERR_NAME;
    if(!fs_ready)return ERR_IO;
    int index=file_find(request.name);if(index<0)return ERR_NOT_FOUND;
    i64 size=file_read(index,executable,FS_MAX_SIZE);if(size<0)return size;
    if(size<(i64)sizeof(ElfHeader))return ERR_FORMAT;
    ElfHeader *h=(ElfHeader *)executable;
    if(h->ident[0]!=127||h->ident[1]!='E'||h->ident[2]!='L'||h->ident[3]!='F'||
       h->ident[4]!=2||h->ident[5]!=1||h->ident[6]!=1||h->type!=2||h->machine!=62||h->version!=1||
       h->ehsize!=sizeof(*h)||h->phentsize!=sizeof(ElfSegment)||!h->phnum||h->phnum>16||
       h->phoff>(u64)size||(u64)h->phnum*sizeof(ElfSegment)>(u64)size-h->phoff)return ERR_FORMAT;
    /* Validate all segments before allocating a slot or copying any bytes. */
    ElfSegment segments[16];memcpy(segments,executable+h->phoff,h->phnum*sizeof(ElfSegment));
    u8 pages[448];memset(pages,0,sizeof(pages));int entry_ok=0;
    for(int i=0;i<h->phnum;i++) {
        ElfSegment *p=&segments[i];
        if(p->type==2||p->type==3||p->type==7)return ERR_FORMAT; /* No dynamic linker or TLS. */
        if(p->type!=1)continue;
        if(p->filesz>p->memsz||p->offset>(u64)size||p->filesz>(u64)size-p->offset||
           p->vaddr<USER_BASE||p->vaddr>=0x5c0000||p->memsz>0x5c0000-p->vaddr||
           (p->flags&~7U)||!(p->flags&4)||((p->flags&3)==3)||
           (p->vaddr&4095)||(p->offset&4095)||p->align!=4096)return ERR_FORMAT;
        if(!p->memsz)continue;
        for(u64 page=(p->vaddr-USER_BASE)/4096;page<(p->vaddr+p->memsz-USER_BASE+4095)/4096;page++) {
            if(pages[page])return ERR_FORMAT;pages[page]=(u8)p->flags;
        }
        if((p->flags&1)&&h->entry>=p->vaddr&&h->entry-p->vaddr<p->filesz)entry_ok=1;
    }
    if(!entry_ok)return ERR_FORMAT;
    /* Legacy applications keep their fixed page-table and RAM reservations. */
    int id;for(id=APP_FIRST;id<LEGACY_TASKS;id++)if(application_slot_available(id))break;
    if(id==LEGACY_TASKS)return ERR_LIMIT;
    create_task(id,executable,0,USER_BASE,USER_BASE,0);
    u64 *pt=user_table(id);
    for(int i=0;i<496;i++)pt[i]=0;
    for(int i=0;i<448;i++)if(pages[i])pt[i]=(physical(id)+(u64)i*4096)|PRESENT|USER|((pages[i]&2)?WRITE:0)|((pages[i]&1)?0:NX);
    for(int i=0;i<h->phnum;i++)if(segments[i].type==1&&segments[i].filesz)
        memcpy((void *)(physical(id)+segments[i].vaddr-USER_BASE),executable+segments[i].offset,segments[i].filesz);
    for(int i=464;i<466;i++)pt[i]=(physical(id)+(u64)i*4096)|PRESENT|USER|NX;
    memcpy((void *)(physical(id)+0x1d1100),request.args,128);
    tasks[id].frame.rip=h->entry;exit_codes[id]=0;task_faults[id]=0;task_fault_addresses[id]=0;
    serial("EXEC: ");serial(request.name);serial("\r\n");return id;
}
