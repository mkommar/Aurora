typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;
#define NATIVE_FILE_CACHE 256
#define APP_FIRST 1
#define TASK_COUNT 4
#define NATIVE_FDS 4
typedef struct {int kind,index;} NativeFd;
typedef struct {NativeFd *fd;} NativeProcess;
typedef struct {u32 kind;u8 pad[16];} File;
static File NFILES[NATIVE_FILE_CACHE];
static NativeProcess native_process[TASK_COUNT];
static NativeFd descriptors[TASK_COUNT][NATIVE_FDS];
static u8 native_active[TASK_COUNT];
static int ext2_ready;
static u32 native_count;
static void *memset(void *out,int value,u64 size){u8 *p=out;while(size--)*p++=value;return out;}
#include "../src/native_cache.h"
__declspec(dllexport) int cache_test(void){
    memset(NFILES,0,sizeof(NFILES));memset(descriptors,0,sizeof(descriptors));
    native_count=NATIVE_FILE_CACHE;native_cache_holdoff=0;native_cache_evictions=0;
    for(u32 i=0;i<native_count;i++)NFILES[i].kind=1;
    native_active[1]=native_active[2]=1;
    native_process[1].fd=descriptors[1];native_process[2].fd=descriptors[2];
    descriptors[1][0]=(NativeFd){1,250};descriptors[2][0]=(NativeFd){1,10};
    *(u32 *)(NFILES[200].pad+8)=1;
    ext2_ready=0;native_cache_trim();
    if(native_cache_evictions||native_count!=256)return 1;
    ext2_ready=1;native_cache_trim();
    if(native_cache_evictions!=253||native_count!=251)return 2;
    if(!NFILES[10].kind||!NFILES[200].kind||!NFILES[250].kind)return 3;
    if(NFILES[0].kind||NFILES[100].kind||NFILES[255].kind)return 4;
    descriptors[1][0].kind=0;
    for(int i=0;i<40;i++)native_cache_trim();
    if(NFILES[250].kind||!NFILES[200].kind||!NFILES[10].kind)return 5;
    return 0;
}
