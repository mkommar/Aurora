/* Evict only between filesystem operations, never during path resolution or
 * exec's multi-file ELF loads. Open descriptors pin indexes; parked recovery
 * entries also remain pinned until their deletion succeeds. */
static u32 native_cache_holdoff;
volatile u64 native_cache_evictions;
static void native_cache_trim(void){
    if(!ext2_ready)return; /* The original raw format stores its directory here. */
    if(native_cache_holdoff){native_cache_holdoff--;return;}
    if(native_count<NATIVE_FILE_CACHE-128)return;
    u8 pinned[(NATIVE_FILE_CACHE+7)/8];memset(pinned,0,sizeof(pinned));
    for(u32 task=APP_FIRST;task<TASK_COUNT;task++)if(native_active[task]&&native_process[task].fd){
        for(int fd=0;fd<NATIVE_FDS;fd++){NativeFd *f=&native_process[task].fd[fd];
            if(f->kind==1&&f->index>=0&&(u32)f->index<native_count)pinned[f->index/8]|=1U<<(f->index%8);
        }
    }
    u32 freed=0;
    for(u32 i=0;i<native_count;i++)if(NFILES[i].kind&&!(pinned[i/8]&(1U<<(i%8)))&&!*(u32 *)(NFILES[i].pad+8)){
        NFILES[i].kind=0;freed++;
    }
    while(native_count&&!NFILES[native_count-1].kind)native_count--;
    native_cache_evictions+=freed;
    /* At most eight new indexes are needed by one filesystem operation
     * (including the bounded script/interpreter chain). Reserve that headroom. */
    native_cache_holdoff=freed/8;if(native_cache_holdoff>512)native_cache_holdoff=512;
}
