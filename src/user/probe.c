/* Included only in the self-test image. These processes deliberately misbehave. */
#include "lib.h"
void user_main(void){
    switch(BOOT->id){
    case 3: *(volatile u64 *)0x10000=0;break; /* Kernel memory: supervisor-only. */
    case 4: *(volatile u64 *)0x2000000=0;break; /* Another task's physical alias. */
    case 5: __asm__ volatile("outb %%al,$0x64"::"a"((u8)0));break;
    case 6: {
        Message m={0};
        if(syscall(SYS_IN,0x64,0,0)!=ERR_CAP ||
           syscall(SYS_OUT,0x604,0x2000,2)!=ERR_CAP ||
           send(DESKTOP,&m)!=ERR_CAP ||
           syscall(SYS_RECV,0x10000,0,0)!=ERR_POINTER ||
           syscall(SYS_RECV,USER_BASE,0,0)!=ERR_POINTER ||
           syscall(SYS_RECV,0x5efff0,0,0)!=ERR_POINTER ||
           syscall(SYS_RECV,0x5ffff0,0,0)!=ERR_POINTER ||
           syscall(SYS_LOG,~0ULL-8,40,0)!=ERR_POINTER ||
           syscall(SYS_LOG,USER_BASE,~0ULL,0)!=ERR_POINTER ||
           syscall(999,0,0,0)!=ERR_SYSCALL){serial("PROBE: syscall validation FAILED\r\n");return;}
        serial("PROBE: syscall validation PASS; spinning without yielding\r\n");
        for(;;)__asm__ volatile("pause");
    }
    case 7: {volatile u8 *p=(volatile u8 *)0x5f0000;*p=0xc3;((void(*)(void))p)();break;}
    case 8: *(volatile u8 *)USER_BASE=0;break; /* Text is read-only. */
    case 9: __asm__ volatile("ud2");break;
    }
    serial("PROBE: expected fault did not occur\r\n");
}
