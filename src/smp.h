extern const u8 ap_start_image[],ap_start_end[];
extern void cpu_idle(void) __attribute__((noreturn));
static int table_checksum(const void *p,u32 length){const u8 *b=p;u8 sum=0;while(length--)sum+=*b++;return !sum;}
static u32 acpi_madt(void){
    u32 ranges[4]={(*(volatile u16 *)0x40e)*16U,1024,0xe0000,0x20000};
    for(int range=0;range<4;range+=2)for(u32 a=ranges[range];a&&a<ranges[range]+ranges[range+1];a+=16){
        const u8 *r=(void *)(u64)a;if(__builtin_memcmp(r,"RSD PTR ",8)||!table_checksum(r,20))continue;
        u32 root=*(const u32 *)(r+16);if(root<0x100000||root>0xfffff000U)continue;
        const u8 *rsdt=(void *)(u64)root;u32 length=*(const u32 *)(rsdt+4);
        if(__builtin_memcmp(rsdt,"RSDT",4)||length<36||length>65536||root>0xffffffffU-length||!table_checksum(rsdt,length))continue;
        for(u32 off=36;off+4<=length;off+=4){u32 address=*(const u32 *)(rsdt+off);if(address<0x100000||address>0xfffff000U)continue;
            const u8 *table=(void *)(u64)address;u32 size=*(const u32 *)(table+4);
            if(!__builtin_memcmp(table,"APIC",4)&&size>=44&&size<=65536&&address<=0xffffffffU-size&&table_checksum(table,size))return address;}
    }
    return 0;
}
static void cpu_ipi(u32 destination,u32 value){
    if(!local_apic)return;
    while(local_apic[0x300/4]&(1<<12))__asm__ volatile("pause");
    local_apic[0x310/4]=destination<<24;local_apic[0x300/4]=value;
    while(local_apic[0x300/4]&(1<<12))__asm__ volatile("pause");
}
/* PIT channel 2 supplies a boot-time delay independent of CPU frequency. */
static void boot_delay(u32 microseconds){
    u32 count=(microseconds*1193+999)/1000;if(count>65535)count=65535;
    u8 control=inb(0x61);outb(0x61,(control&~2U)|1);outb(0x43,0xb0);outb(0x42,count);outb(0x42,count>>8);
    for(u32 timeout=0;timeout<10000000&&!(inb(0x61)&32);timeout++)__asm__ volatile("pause");outb(0x61,control);
}
static void ap_main(u32 index){
    tables_init(index);local_apic[0xf0/4]=0x100|63;
    local_apic[0x350/4]=local_apic[0x360/4]=1U<<16;
    __atomic_store_n(&cpus[index].online,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&kernel_started,__ATOMIC_ACQUIRE))__asm__ volatile("pause");
    enter_user(schedule());
}
void cpu_idle_interrupt(Frame *frame){
    if(frame->vector!=62&&frame->vector!=63)trap_dispatch(frame);
    if(frame->vector!=63&&local_apic)local_apic[0xb0/4]=0;
}
static void smp_init(void){
    cpus[0].online=1;
    u32 lo,hi;__asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(0x1b));
    if(hi||(lo&(1U<<10))||!(lo&0xfffff000U))return;
    local_apic=(volatile u32 *)(u64)(lo&0xfffff000U);wrmsr(0x1b,lo|(1U<<11));local_apic[0xf0/4]=0x100|63;
    cpus[0].apic_id=local_apic[0x20/4]>>24;
    u32 address=acpi_madt();if(!address){serial("SMP: no usable MADT; one CPU\r\n");return;}
    const u8 *table=(void *)(u64)address;u32 length=*(const u32 *)(table+4);
    for(u32 off=44;off+2<=length;){u32 size=table[off+1];if(size<2||size>length-off)break;
        if(!table[off]&&size>=8&&(*(const u32 *)(table+off+4)&1)&&table[off+3]!=cpus[0].apic_id&&cpu_count<CPU_MAX){
            u32 apic=table[off+3];int duplicate=0;for(u32 i=1;i<cpu_count;i++)if(cpus[i].apic_id==apic)duplicate=1;
            if(!duplicate)cpus[cpu_count++].apic_id=apic;
        }off+=size;
    }
    memcpy((void *)0x8000,ap_start_image,ap_start_end-ap_start_image);
    for(u32 i=1;i<cpu_count;i++){
        *(volatile u64 *)0x8f00=0x00300000ULL+(i+1)*0x10000ULL;*(volatile u64 *)0x8f08=(u64)ap_main;*(volatile u32 *)0x8f10=i;
        __asm__ volatile("mfence":::"memory");cpu_ipi(cpus[i].apic_id,0xc500);boot_delay(10000);
        cpu_ipi(cpus[i].apic_id,0x8500);cpu_ipi(cpus[i].apic_id,0x608);boot_delay(200);
        if(!cpus[i].online)cpu_ipi(cpus[i].apic_id,0x608);
        for(u32 attempts=0;attempts<1000&&!__atomic_load_n(&cpus[i].online,__ATOMIC_ACQUIRE);attempts++)boot_delay(100);
        if(!cpus[i].online)panic("AP startup timeout");cpu_online++;
    }
    serial("SMP: online CPUs=");hex(cpu_online);serial("\r\n");
}
