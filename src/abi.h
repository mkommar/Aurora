#ifndef AURORA_ABI_H
#define AURORA_ABI_H
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long i64;

enum { DESKTOP, INPUT, DISPLAY };
enum { SYS_YIELD, SYS_SEND, SYS_RECV, SYS_POLL, SYS_IN, SYS_OUT,
       SYS_LOG, SYS_TICKS, SYS_EXIT, SYS_FILE_READ, SYS_FILE_WRITE,
       SYS_FILE_LIST, SYS_SPAWN, SYS_STATUS, SYS_NATIVE_SPAWN,
       SYS_NATIVE_READ, SYS_NATIVE_WRITE, SYS_NATIVE_LIST, SYS_NATIVE_INPUT, SYS_SYNC };
enum { MSG_KEY=1, MSG_MOUSE, MSG_CLOCK, MSG_PRESENT, MSG_PRESENTED, MSG_POWER };
enum { MSG_CONSOLE=7 };
enum { ERR_NOT_FOUND=-7, ERR_IO=-8, ERR_FORMAT=-9, ERR_LIMIT=-10, ERR_NAME=-11 };
/* Flat AuroraFS: 32 named files, each with a dedicated 64 KiB extent.
   File calls transfer whole files; a read returns up to capacity bytes. */
enum { FS_FILES=32, FS_MAX_SIZE=65536, FS_LBA=512, FS_DATA_LBA=520 };
typedef struct { char name[32]; u32 size,used; u8 reserved[24]; } FileEntry;
typedef struct { char name[32]; u64 buffer,size; } FileRequest;
typedef struct { char name[32]; char args[128]; } SpawnRequest;
enum { ERR_CAP=-1, ERR_POINTER=-2, ERR_FULL=-3, ERR_DEAD=-4, ERR_SYSCALL=-5,
       ERR_EMPTY=-6 };
enum { USER_BASE=0x400000, USER_SIZE=0x200000, BOOT_ADDRESS=0x5d0000,
       SURFACE_ADDRESS=0x1000000, SURFACE_BYTES=1024*768*4 };
typedef struct { u64 sender, type, a, b, c; } Message;
typedef struct { u64 id, framebuffer, pitch; u8 font[4096]; } BootInfo;
#define BOOT ((const BootInfo *)BOOT_ADDRESS)
#endif
