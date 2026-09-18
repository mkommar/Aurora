#include <aurora.h>
int main(int argc,char **argv){
    (void)argc;(void)argv;const char *message="Saved by a C application. This survives reboot.\n";
    char *buffer=malloc(128);if(!buffer)return 1;
    if(aurora_writefile("message.txt",message,strlen(message))<0){puts("Write failed");free(buffer);return 2;}
    long long n=aurora_readfile("message.txt",buffer,127);if(n<0){free(buffer);return 3;}
    buffer[n]=0;printf("Read back: %s",buffer);free(buffer);return 0;
}
