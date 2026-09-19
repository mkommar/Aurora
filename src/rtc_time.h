/* Read UTC once at boot, before the input service starts using the CMOS ports. */
static u64 native_epoch_base;
static u8 native_cmos(u8 reg){outb(0x70,reg);return inb(0x71);}
static u32 native_bcd(u32 value){return (value>>4)*10+(value&15);}
static int native_leap(u32 year){return !(year%4)&&((year%100)||!(year%400));}
static u64 native_epoch(u32 year,u32 month,u32 day,u32 hour,u32 minute,u32 second);
static void native_clock_init(void){
    for(u32 i=0;i<100000&&(native_cmos(10)&128);i++){}
    u32 second=native_cmos(0),minute=native_cmos(2),hour=native_cmos(4),day=native_cmos(7),month=native_cmos(8),year=native_cmos(9),mode=native_cmos(11);
    u32 pm=hour&128;hour&=127;
    if(!(mode&4)){second=native_bcd(second);minute=native_bcd(minute);hour=native_bcd(hour);day=native_bcd(day);month=native_bcd(month);year=native_bcd(year);}
    if(!(mode&2))hour=hour%12+(pm?12:0);year+=2000;
    if(month<1||month>12||day<1||day>31||hour>23||minute>59||second>59)return;
    native_epoch_base=native_epoch(year,month,day,hour,minute,second);
}
static u32 native_timestamp(void){return (u32)(native_epoch_base+timer_ticks/100);}
/* Civil time to Unix seconds and back; shared with FAT timestamps. */
static u64 native_epoch(u32 year,u32 month,u32 day,u32 hour,u32 minute,u32 second){
    const u32 lengths[]={31,28,31,30,31,30,31,31,30,31,30,31};u64 days=0;
    for(u32 y=1970;y<year;y++)days+=365+native_leap(y);
    for(u32 m=1;m<month&&m<=12;m++)days+=lengths[m-1]+(m==2&&native_leap(year));days+=day-1;
    return days*86400+hour*3600+minute*60+second;
}
static void native_civil(u64 epoch,u32 *year,u32 *month,u32 *day,u32 *hour,u32 *minute,u32 *second){
    const u32 lengths[]={31,28,31,30,31,30,31,31,30,31,30,31};u64 days=epoch/86400,rest=epoch%86400;
    *hour=(u32)(rest/3600);*minute=(u32)(rest%3600/60);*second=(u32)(rest%60);u32 y=1970;
    while(days>=365u+native_leap(y)){days-=365u+native_leap(y);y++;}u32 m=1;
    while(days>=lengths[m-1]+(m==2&&native_leap(y))){days-=lengths[m-1]+(m==2&&native_leap(y));m++;}
    *year=y;*month=m;*day=(u32)days+1;
}
