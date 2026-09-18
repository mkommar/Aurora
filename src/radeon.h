/* Safe Radeon discovery. Generation-specific GPU command processors remain
 * disabled; scanout uses the verified VBE framebuffer path. */
static volatile u64 radeon_frames, radeon_pixels;
static int radeon_navi32;
static int radeon_capability(u32 device,u32 id,u32 wanted){
    u32 cap=pci_read(device,0x34)&0xfc;
    for(int hops=0;cap>=0x40&&cap<=0xfc&&hops<48;hops++){
        u32 value=pci_read(device,cap);if((value&255)==id)return cap;
        u32 next=(value>>8)&0xfc;if(next==cap)break;cap=next;
    }
    (void)wanted;return 0;
}
static int __attribute__((unused)) radeon_reset(void){
    /* PCIe FLR is the only generation-neutral reset. Never touch a legacy
     * Radeon reset register from this generic path. */
    if(!radeon_flr)return 0;
    u32 device=radeon_flr;u32 control=pci_read(device,0x08);(void)control;
    u32 cap=pci_read(device,0x34)&0xfc;for(int hops=0;cap>=0x40&&cap<=0xfc&&hops<48;hops++){
        u32 value=pci_read(device,cap);if((value&255)==0x10){pci_write16(device,cap+8,0x8000);for(volatile u32 wait=0;wait<1000000;wait++)__asm__ volatile("pause");return 1;}
        u32 next=(value>>8)&0xfc;if(next==cap)break;cap=next;
    }return 0;
}
static void radeon_init(void){
    for(u32 bus=0;bus<256;bus++)for(u32 slot=0;slot<32;slot++){
        u32 address=(bus<<16)|(slot<<11),id=pci_read(address,0);
        if((id&0xffff)!=0x1002||((pci_read(address,8)>>24)!=3))continue;
        radeon_device=(u16)(id>>16);radeon_bar0=pci_read(address,0x10);
        if(radeon_bar0&1||!(radeon_bar0&0xfffffff0U)){serial("RADEON: unsafe MMIO BAR; fallback\r\n");return;}
        radeon_mmio=radeon_bar0&~15U;u32 bar_hi=pci_read(address,0x14);
        if((radeon_bar0&6)==4)radeon_mmio|=(u64)bar_hi<<32;
        radeon_vram=0;radeon_irq=pci_read(address,0x3c)&255;
        radeon_navi32=radeon_device==RADEON_RX7800XT_DEVICE;
        radeon_generation=radeon_navi32?RADEON_GEN_RDNA3:radeon_device<0x1000?RADEON_GEN_LEGACY:radeon_device<0x4000?RADEON_GEN_R300:radeon_device<0x6800?RADEON_GEN_R600:RADEON_GEN_GCN;
        radeon_flr=address;radeon_irq_cap=radeon_capability(address,0x11,0)!=0||radeon_capability(address,5,0)!=0;
        radeon_present=radeon_navi32;
        if(!radeon_present){serial("RADEON: unsupported generation; VBE fallback\r\n");return;}
        serial("RADEON: display controller detected device=");hex(radeon_device);
        serial(" MMIO; capability service handoff\r\n");return;
    }
    serial("RADEON: no compatible Radeon controller; VBE framebuffer path\r\n");
}
