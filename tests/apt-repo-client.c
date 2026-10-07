#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int get_file(unsigned short port,const char *path,char *body,size_t capacity,size_t *length){
    int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return 0;
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(port)};
    if(inet_pton(AF_INET,"10.0.2.2",&address.sin_addr)!=1||connect(fd,(struct sockaddr *)&address,sizeof(address))){close(fd);return 0;}
    char request[512];int size=snprintf(request,sizeof(request),"GET %s HTTP/1.1\r\nHost: aurora-repo\r\nConnection: close\r\n\r\n",path);
    const char *p=request;int left=size;while(left>0){ssize_t n=write(fd,p,(size_t)left);if(n<0&&errno==EINTR)continue;if(n<=0){close(fd);return 0;}p+=n;left-=(int)n;}
    char response[32768];size_t used=0;ssize_t n;while((n=read(fd,response+used,sizeof(response)-used-1))>0){used+=(size_t)n;if(used==sizeof(response)-1)break;}
    close(fd);if(n<0)return 0;response[used]=0;
    char *separator=strstr(response,"\r\n\r\n");if(!separator||strncmp(response,"HTTP/1.1 200",12))return 0;
    char *content=strstr(response,"Content-Length: ");if(!content||content>separator)return 0;
    size_t expected=(size_t)strtoull(content+16,0,10);char *payload=separator+4;size_t actual=used-(size_t)(payload-response);
    if(actual!=expected||actual>=capacity)return 0;
    memcpy(body,payload,actual);*length=actual;return 1;
}
int main(int argc,char **argv){
    if(argc!=2)return 2;
    unsigned long port=strtoul(argv[1],0,10);if(!port||port>65535)return 2;
    char packages[8192],release[4096],deb[4096];size_t psize,rsize,dsize;
    if(!get_file((unsigned short)port,"/dists/stable/main/binary-musl/Packages",packages,sizeof(packages),&psize))return 3;
    if(!strstr(packages,"Package: aurora-apt-probe")||!strstr(packages,"Filename: pool/aurora-apt-probe_1.0-1_musl-linux-amd64.deb"))return 4;
    if(!get_file((unsigned short)port,"/dists/stable/Release",release,sizeof(release),&rsize))return 5;
    if(!strstr(release,"Architectures: musl-linux-amd64")||!strstr(release,"Components: main"))return 6;
    if(!get_file((unsigned short)port,"/pool/aurora-apt-probe_1.0-1_musl-linux-amd64.deb",deb,sizeof(deb),&dsize))return 7;
    if(dsize<8||memcmp(deb,"!<arch>\n",8))return 8;
    puts("APT_REPO_CLIENT_PASS fetched Packages, Release, and Debian archive via peer Aurora");return 0;
}
