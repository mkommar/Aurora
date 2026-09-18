/* The kernel maps ELF images; musl owns relocation, TLS and library policy. */
typedef struct {ElfHeader header;ElfSegment segments[32];u64 bias,entry,phaddr,end,pages;} NativeElf;
static i64 native_elf_read(int file,u64 base,u64 limit,NativeElf *image,char *interpreter){
    memset(image,0,sizeof(*image));interpreter[0]=0;
    ElfHeader *h=&image->header;u64 length=NFILES[file].size;
    if(native_read(file,0,h,sizeof(*h))!=sizeof(*h))return -8;
    if(h->ident[0]!=127||h->ident[1]!='E'||h->ident[2]!='L'||h->ident[3]!='F'||h->ident[4]!=2||h->ident[5]!=1||h->ident[6]!=1||
       (h->type!=2&&h->type!=3)||h->machine!=62||h->phentsize!=sizeof(ElfSegment)||!h->phnum||h->phnum>32||
       h->phoff>length||h->phnum*sizeof(ElfSegment)>length-h->phoff)return -8;
    if(native_read(file,h->phoff,image->segments,h->phnum*sizeof(ElfSegment))!=(i64)(h->phnum*sizeof(ElfSegment)))return -5;
    image->bias=h->type==3?base:0;
    if(h->entry>=limit-image->bias)return -8;
    image->entry=h->entry+image->bias;int entry_mapped=0;
    for(int i=0;i<h->phnum;i++){
        ElfSegment *s=&image->segments[i];
        if(s->type==3){
            if(interpreter[0]||s->filesz<2||s->filesz>256||s->offset>length||s->filesz>length-s->offset)return -8;
            if(native_read(file,s->offset,interpreter,s->filesz)!=(i64)s->filesz)return -5;
            if(interpreter[0]!='/'||interpreter[s->filesz-1]||ns_length(interpreter)!=s->filesz-1)return -8;
        }
        if(s->type!=1)continue;
        if(s->vaddr>=limit-image->bias)return -8;s->vaddr+=image->bias;
        if(s->vaddr<base||s->memsz>limit-s->vaddr||s->filesz>s->memsz||s->offset>length||s->filesz>length-s->offset||
           ((s->flags&3)==3)||(s->flags&~7U)||!(s->flags&4)||((s->vaddr^s->offset)&4095)||
           (s->align>1&&((s->align&(s->align-1))||((s->vaddr-image->bias-s->offset)&(s->align-1)))))return -8;
        for(int j=0;j<i;j++){ElfSegment *p=&image->segments[j];
            if(p->type==1&&s->memsz&&p->memsz&&(s->vaddr&~4095ULL)<((p->vaddr+p->memsz+4095)&~4095ULL)&&
               (p->vaddr&~4095ULL)<((s->vaddr+s->memsz+4095)&~4095ULL))return -8;}
        if((s->flags&1)&&image->entry>=s->vaddr&&image->entry-s->vaddr<s->filesz)entry_mapped=1;
        if(h->phoff>=s->offset&&h->phoff-s->offset<=s->filesz&&h->phnum*sizeof(ElfSegment)<=s->filesz-(h->phoff-s->offset))image->phaddr=s->vaddr+h->phoff-s->offset;
        if(s->vaddr+s->memsz>image->end)image->end=s->vaddr+s->memsz;
        if(s->memsz)image->pages+=(s->vaddr+s->memsz+4095)/4096-s->vaddr/4096;
    }
    return entry_mapped&&image->phaddr?0:-8;
}
static i64 native_elf_map(u32 id,int file,NativeElf *image){
    for(int i=0;i<image->header.phnum;i++){ElfSegment *s=&image->segments[i];if(s->type!=1||!s->memsz)continue;
        if(!native_map(id,s->vaddr,s->memsz,((s->flags&2)?WRITE:0)|((s->flags&1)?0:NX)))return -12;
        if(s->filesz&&native_read(file,s->offset,(void *)(native_phys(id)+s->vaddr-USER_BASE),s->filesz)!=(i64)s->filesz)return -5;
    }return 0;
}
