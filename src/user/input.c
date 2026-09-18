/* User-mode PS/2 and platform service. Port capabilities are checked by the kernel. */
#include "lib.h"
static void wait_write(void){for(int i=0;i<100000;i++)if(!(inb(0x64)&2))return;}
static void pscommand(u8 b){wait_write();outb(0x64,b);}
static void psdata(u8 b){wait_write();outb(0x60,b);}
static u8 psread(void){for(int i=0;i<100000;i++)if(inb(0x64)&1)return inb(0x60);return 0;}
static void mouse_command(u8 b){pscommand(0xd4);psdata(b);psread();}
static void input_init(void){pscommand(0xad);pscommand(0xa7);while(inb(0x64)&1)inb(0x60);pscommand(0x20);u8 c=psread();pscommand(0x60);psdata((c|0x40)&~0x33);pscommand(0xae);pscommand(0xa8);mouse_command(0xf6);mouse_command(0xf4);}
static u8 rtc(u8 reg){outb(0x70,reg);return inb(0x71);}
static int decode(u8 v,int binary){return binary?v:(v>>4)*10+(v&15);}
static void event(u64 type,u64 a,u64 b,u64 c){Message m={0,type,a,b,c};while(send(DESKTOP,&m)==ERR_FULL)yield();}
void user_main(void){
    input_init();serial("INPUT: ring3 PS/2 + platform service ready\r\n");
    u64 next_clock=0;u8 packet[3];int pos=0;
    for(;;){
        Message request;
        while(poll(&request)==0)if(request.sender==DESKTOP&&request.type==MSG_POWER){
            if(request.a==1)pscommand(0xfe);
            if(request.a==2)outw(0x604,0x2000);
        }
        for(int i=0;i<64;i++){
            u8 s=inb(0x64);if(!(s&1))break;u8 b=inb(0x60);
            if(s&32){
                if(!pos&&!(b&8))continue;
                packet[pos++]=b;
                if(pos==3){pos=0;event(MSG_MOUSE,packet[0],packet[1],packet[2]);}
            }else event(MSG_KEY,b,0,0);
        }
        if(ticks()>=next_clock){
            next_clock=ticks()+100;
            if(!(rtc(0x0a)&0x80)){
                u8 mode=rtc(0x0b),raw=rtc(4);int hour=decode(raw&0x7f,mode&4);
                if(!(mode&2))hour=hour%12+((raw&0x80)?12:0);
                event(MSG_CLOCK,hour,decode(rtc(2),mode&4),0);
            }
        }
        yield();
    }
}
