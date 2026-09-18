/* Bounded PCI capability discovery and one BSP-targeted message interrupt.
 * MMIO stays supervisor-only. Drivers receive no arbitrary user port grants. */
static volatile u32 *local_apic;
static u32 pci_msix_cap;
static void pci_write(u32 device,u32 reg,u32 value){io_write32(0xcf8,0x80000000U|device|(reg&~3U));io_write32(0xcfc,value);}
static void pci_write16(u32 device,u32 reg,u16 value){io_write32(0xcf8,0x80000000U|device|(reg&~3U));outw(0xcfc+(reg&2),value);}
static int pci_message_irq(u32 device,u32 vector){
    u32 lo,hi;__asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(0x1b));
    if(hi||(lo&(1U<<10)))return 0;u64 base=lo&0xfffff000U;if(!base)return 0;
    local_apic=(volatile u32 *)base;wrmsr(0x1b,(u64)lo|(1U<<11));local_apic[0xf0/4]=0x100|63;
    if(!(pci_read(device,4)&(1U<<20)))return 0;
    u32 msi=0,msix=0,cap=pci_read(device,0x34)&0xfc;
    for(int hops=0;cap>=0x40&&cap<=0xfc&&hops<48;hops++){
        u32 value=pci_read(device,cap);if((value&255)==0x11)msix=cap;if((value&255)==5)msi=cap;
        u32 next=(value>>8)&0xfc;if(next==cap)break;cap=next;
    }
    u32 address=0xfee00000U|((local_apic[0x20/4]>>24)<<12);
    if(msix){u32 table=pci_read(device,msix+4),bir=table&7;if(bir>5)return 0;
        u32 bar=pci_read(device,0x10+bir*4);if(bar&1)return 0;
        u64 physical=bar&~15U;if((bar&6)==4){if(bir==5)return 0;physical|=(u64)pci_read(device,0x14+bir*4)<<32;}
        physical+=table&~7U;if(physical<0x80000000ULL||physical>0xfffffff0ULL)return 0;
        u16 control=pci_read(device,msix)>>16;pci_write16(device,msix+2,control|0xc000);
        volatile u32 *entry=(volatile u32 *)physical;entry[3]=1;entry[0]=address;entry[1]=0;entry[2]=vector;
        __asm__ volatile("mfence":::"memory");entry[3]=0;pci_write16(device,msix+2,(control|0x8000)&~0x4000);
        pci_msix_cap=msix;pci_write16(device,4,(pci_read(device,4)&0xffff)|0x400);return 2;
    }
    if(msi){u16 control=pci_read(device,msi)>>16;pci_write(device,msi+4,address);
        if(control&128){pci_write(device,msi+8,0);pci_write16(device,msi+12,vector);}else pci_write16(device,msi+8,vector);
        pci_write16(device,msi+2,(control&~0x70)|1);pci_write16(device,4,(pci_read(device,4)&0xffff)|0x400);return 1;}
    return 0;
}
