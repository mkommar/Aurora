/* Read UTC once at boot, before the input service starts using the CMOS ports. */
static u64 native_epoch_base;
static u8 native_cmos(u8 reg){outb(0x70,reg);return inb(0x71);}
static u32 native_bcd(u32 value){return (value>>4)*10+(value&15);}
static int native_leap(u32 year){return !(year%4)&&((year%100)||!(year%400));}
static void native_clock_init(void){
    for(u32 i=0;i<100000&&(native_cmos(10)&128);i++){}
    u32 second=native_cmos(0),minute=native_cmos(2),hour=native_cmos(4),day=native_cmos(7),month=native_cmos(8),year=native_cmos(9),mode=native_cmos(11);
    u32 pm=hour&128;hour&=127;
    if(!(mode&4)){second=native_bcd(second);minute=native_bcd(minute);hour=native_bcd(hour);day=native_bcd(day);month=native_bcd(month);year=native_bcd(year);}
    if(!(mode&2))hour=hour%12+(pm?12:0);year+=2000;
    if(month<1||month>12||day<1||day>31||hour>23||minute>59||second>59)return;
    const u32 lengths[]={31,28,31,30,31,30,31,31,30,31,30,31};u64 days=0;
    for(u32 y=1970;y<year;y++)days+=365+native_leap(y);
    for(u32 m=1;m<month;m++)days+=lengths[m-1]+(m==2&&native_leap(year));days+=day-1;
    native_epoch_base=days*86400+hour*3600+minute*60+second;
}
static u32 native_timestamp(void){return (u32)(native_epoch_base+timer_ticks/100);}
