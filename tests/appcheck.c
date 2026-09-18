#include <aurora.h>
#include "../src/user/lib.h"
static unsigned char file_buffer[65536],read_buffer[65536];
#define CHECK(condition) do {if(!(condition)){printf("RUNTIME FAIL line %d\n",__LINE__);return 1;}}while(0)
int main(int argc,char **argv){
    CHECK(argc==3&&strcmp(argv[1],"alpha")==0&&strcmp(argv[2],"beta")==0);
    CHECK(strlen("hello")==5&&strcmp("a","b")<0&&atoi(" -42")==-42);
    CHECK(malloc(0)==NULL&&malloc(~0ULL)==NULL);
    void *a=malloc(100),*b=malloc(200);CHECK(a&&b&&a!=b);memset(a,0x5a,100);
    free(a);void *c=malloc(80);CHECK(c==a);free(c);free(b);
    void *large=malloc(120000);CHECK(large);free(large);
    for(int i=0;i<65536;i++)file_buffer[i]=(unsigned char)(i*17+3);
    CHECK(aurora_writefile("boundary.bin",file_buffer,sizeof(file_buffer))==65536);
    CHECK(aurora_readfile("boundary.bin",read_buffer,sizeof(read_buffer))==65536);
    for(int i=0;i<65536;i++)CHECK(file_buffer[i]==read_buffer[i]);
    CHECK(aurora_readfile("boundary.bin",read_buffer,7)==7);
    CHECK(aurora_writefile("boundary.bin","short",5)==5);
    CHECK(aurora_readfile("boundary.bin",read_buffer,sizeof(read_buffer))==5);
    CHECK(aurora_writefile("empty.txt",NULL,0)==0);
    CHECK(aurora_readfile("empty.txt",NULL,0)==0);
    CHECK(aurora_readfile("missing.txt",read_buffer,1)==ERR_NOT_FOUND);
    CHECK(aurora_writefile("bad/name",file_buffer,1)==ERR_NAME);
    CHECK(aurora_writefile("boundary.bin",file_buffer,65537)==ERR_LIMIT);
    CHECK(aurora_readfile("boundary.bin",(void *)0x10000,5)==ERR_POINTER);
    CHECK(aurora_readfile("boundary.bin",(void *)0x400000,5)==ERR_POINTER);
    CHECK(aurora_writefile("boundary.bin",(void *)0x5eefff,2)==ERR_POINTER);
    CHECK(syscall(SYS_SPAWN,0,0,0)==ERR_CAP);
    printf("FORMAT %d %u %x %c %%\n",-2147483647-1,42U,255U,'A');
    puts("RUNTIME PASS");return 7;
}
