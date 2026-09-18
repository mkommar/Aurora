#include "lib.h"

enum{W=1024,H=768};
static u32 *back=(u32*)0x1000000;
static int hour,minute;
static void event(const Message *m);
static u64 frame_number;
static int mx=820,my=420,dirty=1,app=0,wx=190,wy=118,drag,dx,dy,buttons,theme;
static int shift,caps,control;
static char notes[1600]="Welcome to your own little operating system.\n\nClick here and start typing.\n\nUse terminal save and load to keep these notes.\n";
static int nlen;
static char cmd[72];static int clen;
static char lines[17][82];static int linecount;
static int running_pid=-1;
static char output_lines[TASK_LIMIT][74];
static int output_lengths[TASK_LIMIT];
static u32 accent=0x55e0c2;
static void rect(int x,int y,int w,int h,u32 c){int x2=x+w,y2=y+h;if(x<0)x=0;if(y<0)y=0;if(x2>W)x2=W;if(y2>H)y2=H;for(int j=y;j<y2;j++)for(int i=x;i<x2;i++)back[j*W+i]=c;}
static void glyph(int x,int y,char ch,u32 c,int scale){const u8*f=BOOT->font+(u8)ch*16;for(int j=0;j<16;j++)for(int i=0;i<8;i++)if(f[j]&(128>>i))rect(x+i*scale,y+j*scale,scale,scale,c);}
static void text(int x,int y,const char*s,u32 c,int scale){while(*s){glyph(x,y,*s++,c,scale);x+=8*scale;}}
static void number(int x,int y,unsigned n,u32 c){char b[12];int i=0;do{b[i++]='0'+n%10;n/=10;}while(n);while(i) {glyph(x,y,b[--i],c,1);x+=8;}}
static int hit(int x,int y,int w,int h){return mx>=x&&my>=y&&mx<x+w&&my<y+h;}
static void logline(const char*s){if(linecount==17){for(int i=0;i<16;i++)memcpy(lines[i],lines[i+1],82);linecount=16;}int n=len(s);if(n>81)n=81;memcpy(lines[linecount],s,n);lines[linecount++][n]=0;}
static void console_text(int id,const char *s,int count){
 for(int i=0;i<count;i++){char c=s[i];if(c=='\r')continue;
  if(c=='\b'){if(output_lengths[id])output_lengths[id]--;}
  else if(c=='\n'){output_lines[id][output_lengths[id]]=0;logline(output_lines[id]);output_lengths[id]=0;}
  else {output_lines[id][output_lengths[id]++]=(c>=32&&c<127)?c:'?';if(output_lengths[id]==73){output_lines[id][73]=0;logline(output_lines[id]);output_lengths[id]=0;}}
 }dirty=1;
}
static void report_error(i64 error){
 const char *s="Operation failed.";
 if(error==ERR_NOT_FOUND)s="File not found.";else if(error==ERR_FORMAT)s="Invalid or unsupported ELF application.";
 else if(error==ERR_LIMIT)s="File, memory, or process limit reached.";else if(error==ERR_IO)s="Disk I/O error or filesystem unavailable.";
 else if(error==ERR_NAME)s="Invalid filename (maximum 31 characters).";logline(s);
}
static void check_application(void){
 if(running_pid<0)return;i64 code=0;
 if(syscall(SYS_STATUS,running_pid,(u64)&code,0)==1){
  int id=running_pid;if(output_lengths[id]){output_lines[id][output_lengths[id]]=0;logline(output_lines[id]);output_lengths[id]=0;}
  char b[64]="Application exited: ";int n=len(b);u64 value=code<0?(u64)(-code):(u64)code;if(code<0)b[n++]='-';char digits[24];int k=0;do{digits[k++]='0'+value%10;value/=10;}while(value);while(k)b[n++]=digits[--k];b[n]=0;
  logline(b);serial(b);serial("\r\n");running_pid=-1;dirty=1;
 }
}
static void launch(const char *commandline){
 if(commandline[0]=='.'&&commandline[1]=='/')commandline+=2;
 if(running_pid>=0){logline("An application is still running.");return;}
 SpawnRequest r={0};int n=0;while(commandline[n]&&commandline[n]!=' '&&n<31){r.name[n]=commandline[n];n++;}
 if(commandline[n]&&commandline[n]!=' '){report_error(ERR_NAME);return;}
 int length=len(commandline);if(length>127){report_error(ERR_LIMIT);return;}memcpy(r.args,commandline,length+1);
 i64 id=syscall(SYS_SPAWN,(u64)&r,0,0);if(id==ERR_NOT_FOUND)id=syscall(SYS_NATIVE_SPAWN,(u64)&r,0,0);if(id<0)report_error(id);else running_pid=(int)id;
}
static i64 development_file(const char *name,void *buffer,u64 size,int write){
 FileRequest r={0};int i=0;while(name[i]&&i<31){r.name[i]=name[i];i++;}if(name[i])return ERR_NAME;
 r.buffer=(u64)buffer;r.size=size;return syscall(write?SYS_NATIVE_WRITE:SYS_NATIVE_READ,(u64)&r,0,0);
}
static void openapp(int id){app=id;drag=0;dirty=1;serial("APP ");char label[2]={'0'+id,0};serial(label);serial("\r\n");}
static void command(void){char b[80]=" > ";memcpy(b+3,cmd,clen+1);logline(b);serial("COMMAND ");serial(cmd);serial("\r\n");
 if(eq(cmd,"help")){logline("help about mem clear theme reboot poweroff");logline("ls | cat FILE | save [FILE] | load [FILE] | run APP");logline("GCC: gcc -static demo.c -o demo, then ./demo");logline("GNU environment: bash, make; FAT32: /exchange");logline("Applications: hello [name], calc A B, filedemo");}
 else if(eq(cmd,"about")){logline("Aurora 0.2 / original x86-64 microkernel");logline("Three ring-3 services, private address spaces, IPC.");}
 else if(eq(cmd,"mem")){logline("QEMU: 128 MiB minimal / 1 GiB with native GCC disk");logline("SDK process: 2 MiB; native tool: 128 MiB address space");logline("Desktop + display share a 3 MiB presentation surface.");}
 else if(eq(cmd,"clear"))linecount=0;
 else if(eq(cmd,"theme")){theme=!theme;accent=theme?0xf3bb70:0x55e0c2;logline("Desktop palette changed.");}
 else if(eq(cmd,"reboot")){if(syscall(SYS_SYNC,0,0,0)<0)logline("Disk flush failed; reboot cancelled.");else{Message m={0,MSG_POWER,1,0,0};send(INPUT,&m);}}
 else if(eq(cmd,"poweroff")){if(syscall(SYS_SYNC,0,0,0)<0)logline("Disk flush failed; poweroff cancelled.");else{Message m={0,MSG_POWER,2,0,0};send(INPUT,&m);}}
 else if(eq(cmd,"ls")){for(int i=0;i<FS_FILES;i++){FileEntry entry;i64 r=syscall(SYS_FILE_LIST,i,(u64)&entry,0);if(r<0){report_error(r);break;}if(entry.used)logline(entry.name);}for(int i=0;i<4096;i++){FileEntry entry;if(syscall(SYS_NATIVE_LIST,i,(u64)&entry,0)<0)break;logline(entry.name);}}
 else if(cmd[0]=='c'&&cmd[1]=='a'&&cmd[2]=='t'&&cmd[3]==' '){static char buffer[FS_MAX_SIZE];i64 n=file_request(cmd+4,buffer,sizeof(buffer),0);if(n==ERR_NOT_FOUND)n=development_file(cmd+4,buffer,sizeof(buffer),0);if(n<0)report_error(n);else {console_text(0,buffer,(int)n);if(output_lengths[0]){console_text(0,"\n",1);}}}
 else if(cmd[0]=='s'&&cmd[1]=='a'&&cmd[2]=='v'&&cmd[3]=='e'&&cmd[4]==' '){i64 r=development_file(cmd+5,notes,nlen,1);if(r<0)report_error(r);else logline("Source saved to development disk.");}
 else if(cmd[0]=='l'&&cmd[1]=='o'&&cmd[2]=='a'&&cmd[3]=='d'&&cmd[4]==' '){i64 r=development_file(cmd+5,notes,sizeof(notes)-1,0);if(r<0)report_error(r);else{nlen=(int)r;notes[nlen]=0;logline("Source loaded into Notes (F3).");}}
 else if(eq(cmd,"save")){i64 r=file_request("notes.txt",notes,nlen,1);if(r<0)report_error(r);else logline("Notes saved to notes.txt.");}
 else if(eq(cmd,"load")){i64 r=file_request("notes.txt",notes,sizeof(notes)-1,0);if(r<0)report_error(r);else {nlen=(int)r;notes[nlen]=0;logline("Notes loaded from notes.txt.");}}
 else if(cmd[0]=='r'&&cmd[1]=='u'&&cmd[2]=='n'&&cmd[3]==' ')launch(cmd+4);
 else if(clen)launch(cmd);
 clen=0;cmd[0]=0;
}
static const char normal[128]={
 [2]='1',[3]='2',[4]='3',[5]='4',[6]='5',[7]='6',[8]='7',[9]='8',[10]='9',[11]='0',[12]='-',[13]='=',
 [16]='q',[17]='w',[18]='e',[19]='r',[20]='t',[21]='y',[22]='u',[23]='i',[24]='o',[25]='p',[26]='[',[27]=']',
 [30]='a',[31]='s',[32]='d',[33]='f',[34]='g',[35]='h',[36]='j',[37]='k',[38]='l',[39]=';',[40]='\'',[41]='`',[43]='\\',
 [44]='z',[45]='x',[46]='c',[47]='v',[48]='b',[49]='n',[50]='m',[51]=',',[52]='.',[53]='/',[57]=' '};
static const char shifted[128]={[2]='!',[3]='@',[4]='#',[5]='$',[6]='%',[7]='^',[8]='&',[9]='*',[10]='(',[11]=')',[12]='_',[13]='+',[26]='{',[27]='}',[39]=':',[40]='"',[41]='~',[43]='|',[51]='<',[52]='>',[53]='?'};
static void key(u8 sc){if(sc==29){control=1;return;}if(sc==157){control=0;return;}if(sc==42||sc==54){shift=1;return;}if(sc==170||sc==182){shift=0;return;}if(sc&128)return;if(sc==58){caps=!caps;return;}if(sc>=59&&sc<=62){openapp(sc-59);return;}if(sc==1&&running_pid<0){openapp(-1);return;}
 char c=normal[sc];if(c>='a'&&c<='z'){if(shift^caps)c-=32;}else if(shift&&shifted[sc])c=shifted[sc];
 if(app==1&&running_pid>=0){u8 input=sc==28?'\n':sc==14?127:sc==15?'\t':sc==1?27:(u8)c;if(control&&c)input=(u8)c&31;
  if(input&&syscall(SYS_NATIVE_INPUT,running_pid,input,0)!=ERR_NOT_FOUND){dirty=1;return;}}
 if(app==1){if(sc==14&&clen)cmd[--clen]=0;else if(sc==28)command();else if(c&&clen<70){cmd[clen++]=c;cmd[clen]=0;}}
 if(app==2){if(sc==14&&nlen)notes[--nlen]=0;else if((c||sc==28)&&nlen<1598){notes[nlen++]=sc==28?'\n':c;notes[nlen]=0;}}
 dirty=1;
}
static void click(void){if(my>=704&&my<752){for(int i=0;i<4;i++)if(hit(242+i*138,708,128,40)){openapp(i);return;}}
 if(app<0)return;
 if(hit(wx+594,wy,46,36)){openapp(-1);return;}
 if(hit(wx,wy,590,36)){drag=1;dx=mx-wx;dy=my-wy;return;}
 if(app==0&&hit(wx+28,wy+308,228,42))openapp(1);
 if(app==3&&hit(wx+28,wy+135,260,45)){theme=!theme;accent=theme?0xf3bb70:0x55e0c2;}
}
static void mouse(u8 b){static u8 p[3];static int pos;if(!pos&&!(b&8))return;p[pos++]=b;if(pos<3)return;pos=0;if(p[0]&0xc0)return;
 mx+=(int)p[1]-((p[0]&16)?256:0);my-=(int)p[2]-((p[0]&32)?256:0);if(mx<0)mx=0;if(mx>W-1)mx=W-1;if(my<0)my=0;if(my>H-1)my=H-1;
 int now=p[0]&1;if(now&&!buttons)click();if(!now)drag=0;if(drag){wx=mx-dx;wy=my-dy;if(wx<0)wx=0;if(wx>384)wx=384;if(wy<42)wy=42;if(wy>284)wy=284;}buttons=now;dirty=1;
}
static void button(int x,int y,int w,const char*s){rect(x,y,w,42,accent);text(x+16,y+13,s,0x102c30,1);}
static void draw_window(void){if(app<0)return;int x=wx,y=wy;rect(x+7,y+9,640,408,0x071622);rect(x,y,640,408,0xe8eef2);rect(x,y,640,36,0x263e50);rect(x,y,4,36,accent);
 const char*titles[]={"Welcome / Aurora","Terminal / system console","Notes / notebook","Settings / this computer"};text(x+17,y+10,titles[app],0xffffff,1);text(x+610,y+10,"x",0xffb2a7,1);
 if(app==0){text(x+28,y+63,"Hello, Aurora.",0x153248,2);text(x+28,y+108,"A small beginning. An operating system of your own.",0x526473,1);
 rect(x+28,y+150,584,126,0xd7e3e9);text(x+46,y+165,"01  ORIGINAL MICROKERNEL",0x153248,1);text(x+46,y+190,"Protected services. Small kernel. Messages between them.",0x526473,1);text(x+46,y+216,"02  A DESKTOP YOU CAN TOUCH",0x153248,1);text(x+46,y+241,"Move windows. Write a note. Explore the terminal.",0x526473,1);
 button(x+28,y+308,228,"Open the terminal  >");text(x+28,y+373,"F1-F4 switch apps   /   Drag the title bar to move",0x526473,1);}
 if(app==1){rect(x+12,y+48,616,345,0x101f2c);for(int i=0;i<linecount;i++)text(x+24,y+61+i*17,lines[i],0xc5dedf,1);
  if(running_pid>=0){output_lines[running_pid][output_lengths[running_pid]]=0;text(x+24,y+61+linecount*17,output_lines[running_pid],0xffffff,1);rect(x+24+output_lengths[running_pid]*8,y+75+linecount*17,8,2,accent);}
  else{text(x+24,y+61+linecount*17,">",accent,1);text(x+40,y+61+linecount*17,cmd,0xffffff,1);rect(x+40+clen*8,y+75+linecount*17,8,2,accent);}}
 if(app==2){text(x+22,y+51,"Use terminal save / load to keep notes across reboots.",0x526473,1);rect(x+16,y+80,608,282,0xffffff);
 int row=0,col=0,total=0;for(int i=0;i<nlen;i++){if(notes[i]=='\n'){total++;col=0;}else if(++col>=72){total++;col=0;}}
 int skip=total>14?total-14:0;col=0;for(int i=0;i<nlen;i++){if(notes[i]=='\n'){row++;col=0;continue;}if(row>=skip&&row-skip<15)glyph(x+24+col*8,y+88+(row-skip)*17,notes[i],0x263e50,1);if(++col>=72){col=0;row++;}}
 rect(x+24+col*8,y+102+(row-skip)*17,8,2,0x168e81);text(x+22,y+378,"SAVE VIA TERMINAL",0x526473,1);number(x+510,y+378,nlen,0x526473);text(x+548,y+378,"/ 1598",0x526473,1);}
 if(app==3){text(x+28,y+62,"Make it yours.",0x153248,2);text(x+28,y+108,"Choose a desktop palette",0x526473,1);button(x+28,y+135,260,theme?"Palette: warm amber":"Palette: cool mint");
 text(x+28,y+209,"AURORA 0.2",0x153248,1);text(x+28,y+238,"Architecture   x86-64 / long mode",0x526473,1);text(x+28,y+262,"Display        1024 x 768 / 32-bit color",0x526473,1);text(x+28,y+286,"Input          PS/2 keyboard + mouse",0x526473,1);text(x+28,y+310,"Runtime        Ring 3 services / preemptive IPC",0x526473,1);text(x+28,y+363,"AuroraFS storage + C applications. No networking yet.",0x526473,1);}
}
static void draw(void){for(int y=0;y<H;y++){u32 c=theme?((24+y/50)<<16|(28+y/60)<<8|(42+y/35)):((12+y/90)<<16|(30+y/24)<<8|(48+y/16));rect(0,y,W,1,c);}
 for(int i=0;i<9;i++){int x=610+i*46;rect(x,120+i*26,2,500-i*20,theme?0x414252:0x214c61);}
 rect(0,0,W,36,0x102331);text(22,10,"A U R O R A",accent,1);text(180,10,"DESKTOP  /  01",0x8baab9,1);text(772,10,"64-BIT",0x8baab9,1);
 char time[6]={'0'+hour/10,'0'+hour%10,':','0'+minute/10,'0'+minute%10,0};text(943,10,time,0xe8eef2,1);
 text(40,82,"A",accent,4);text(42,156,"AURORA",0xd0e6ec,1);text(42,181,"0.2 / MICRO",0x8baab9,1);
 text(42,635,"Built from the first instruction.",0x9bbbc8,1);text(42,659,"Your machine. Your kernel. Your next idea.",0x7697a8,1);
 draw_window();rect(226,698,570,60,0x071622);const char*names[]={"F1 Welcome","F2 Terminal","F3 Notes","F4 Settings"};for(int i=0;i<4;i++){rect(242+i*138,708,128,40,app==i?0x2c4c5b:0x182f3e);if(app==i)rect(262+i*138,746,88,2,accent);text(250+i*138,721,names[i],app==i?accent:0xb7cbd4,1);}
 for(int j=0;j<18;j++)for(int i=0;i<=j/2;i++){rect(mx+i,my+j,1,1,0x06141d);if(i>0&&i<j/2&&j<15)rect(mx+i,my+j,1,1,0xffffff);}
 dirty=0;
 Message present={0,MSG_PRESENT,++frame_number,0,0};
 while(send(DISPLAY,&present)==ERR_FULL)yield();
 /* Do not write the shared surface until the display acknowledges this frame.
    Input can arrive while waiting, so process it and retain the dirty flag. */
 for(;;){Message m;receive(&m);if(m.sender==DISPLAY&&m.type==MSG_PRESENTED&&m.a==frame_number)break;event(&m);}
}
static void event(const Message *m){
 if(m->sender>=3&&m->sender<TASK_LIMIT&&m->type==MSG_CONSOLE){const char *s=(const char *)&m->a;int n=0;while(n<24&&s[n])n++;console_text((int)m->sender,s,n);return;}
 if(m->sender!=INPUT)return;
 if(m->type==MSG_KEY)key(m->a);
 else if(m->type==MSG_MOUSE){mouse(m->a);mouse(m->b);mouse(m->c);}
 else if(m->type==MSG_CLOCK){hour=m->a;minute=m->b;dirty=1;}
}
void user_main(void){
 nlen=len(notes);logline("Aurora microkernel console [version 0.2]");
 logline("Type help to explore your new OS.");logline("");
 serial("DESKTOP: ring3 GUI ready\r\n");draw();
 serial("AURORA: desktop ready (user mode + display IPC)\r\n");
 for(;;){
   /* Input received during presentation must repaint before blocking again. */
   if(dirty){draw();continue;}
   Message m;receive(&m);event(&m);
   for(int i=0;i<64&&poll(&m)==0;i++)event(&m);
   check_application();
 }
}
