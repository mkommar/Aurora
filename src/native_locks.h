/* Bounded whole-file advisory locks. POSIX locks belong to a process; flock
 * locks belong to an open description and survive dup/fork until last close.
 * The native state lock serializes this table, including waiter checks. */
#define NATIVE_LOCKS 128
typedef struct {u64 key;u32 owner,kind,type;} NativeLock;
static NativeLock native_locks[NATIVE_LOCKS];
static NativeLock *native_lock_conflict(u64 key,u32 owner,u32 kind,u32 type){
    for(u32 i=0;i<NATIVE_LOCKS;i++){NativeLock *lock=&native_locks[i];
        if(lock->type&&lock->key==key&&lock->kind==kind&&lock->owner!=owner&&(type==2||lock->type==2))return lock;}
    return 0;
}
static void native_lock_release(u64 key,u32 owner,u32 kind){
    for(u32 i=0;i<NATIVE_LOCKS;i++){NativeLock *lock=&native_locks[i];
        if(lock->type&&(!key||lock->key==key)&&lock->owner==owner&&lock->kind==kind)lock->type=0;}
}
static int native_lock_set(u64 key,u32 owner,u32 kind,u32 type){
    if(!type){native_lock_release(key,owner,kind);return 0;}
    if(native_lock_conflict(key,owner,kind,type))return -11;
    NativeLock *free=0;
    for(u32 i=0;i<NATIVE_LOCKS;i++){NativeLock *lock=&native_locks[i];
        if(lock->type&&lock->key==key&&lock->owner==owner&&lock->kind==kind){lock->type=type;return 0;}
        if(!lock->type)free=lock;}
    if(!free)return -37;*free=(NativeLock){key,owner,kind,type};return 0;
}
