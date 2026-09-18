/* The only user process granted access to the hardware framebuffer. */
#include "lib.h"
void user_main(void){
    volatile u32 *fb=(volatile u32 *)BOOT->framebuffer;
    const volatile u32 *surface=(const volatile u32 *)SURFACE_ADDRESS;
    serial("DISPLAY: ring3 framebuffer service ready\r\n");
    for(;;){
        Message m;receive(&m);
        if(m.sender!=DESKTOP||m.type!=MSG_PRESENT)continue;
        for(int y=0;y<768;y++)for(int x=0;x<1024;x++)fb[y*BOOT->pitch+x]=surface[y*1024+x];
        Message reply={0,MSG_PRESENTED,m.a,0,0};
        while(send(DESKTOP,&reply)==ERR_FULL)yield();
    }
}
