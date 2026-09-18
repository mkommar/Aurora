/* GPU policy belongs to the isolated display service. The kernel only grants
 * the framebuffer mapping and reports immutable capability bits in BootInfo. */
#define GPU_FEATURE_RADEON_SCANOUT 1ULL
#include "../radeon_service.h"
static RadeonCommandRing *gpu_ring(void){
    return (RadeonCommandRing *)(u64)BOOT->gpu_ring;
}
static int gpu_navi32_ready(void);
static int gpu_submit(u32 opcode,u64 address,u64 value,u64 length){
    RadeonCommandRing *ring=gpu_ring();if(!ring||!gpu_navi32_ready()||!(BOOT->display_features&GPU_FEATURE_RADEON_SCANOUT))return 0;
    u32 producer=ring->producer,consumer=ring->consumer;
    if(producer-consumer>=RADEON_RING_ENTRIES){ring->errors++;return 0;}
    RadeonCommand *command=&ring->commands[producer%RADEON_RING_ENTRIES];
    *command=(RadeonCommand){opcode,0,producer,0,address,value,length};
    __asm__ volatile("mfence":::"memory");ring->producer=producer+1;ring->doorbell=producer+1;return 1;
}
static int gpu_navi32_ready(void){
    return BOOT->radeon_generation==RADEON_GEN_RDNA3&&BOOT->radeon_device==RADEON_RX7800XT_DEVICE&&BOOT->radeon_mmio;
}
static void gpu_present(volatile u32 *framebuffer,const volatile u32 *surface,u64 pitch,u64 features){
    if(features&GPU_FEATURE_RADEON_SCANOUT){
        /* Safe accelerated path: aligned 64-bit scanout copies. Hardware
         * command submission will be added behind this service boundary. */
        for(int y=0;y<768;y++){
            volatile u64 *out=(volatile u64 *)(framebuffer+y*pitch);
            const volatile u64 *in=(const volatile u64 *)(surface+y*1024);
            for(int x=0;x<512;x++)out[x]=in[x];
        }
        gpu_submit(RADEON_CMD_PRESENT,(u64)framebuffer,(u64)surface,(u64)1024*768*4);
    }else for(int y=0;y<768;y++)for(int x=0;x<1024;x++)framebuffer[y*pitch+x]=surface[y*1024+x];
}
