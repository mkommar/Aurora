#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int send_all(int fd,const void *data,size_t size){
    const char *p=data;while(size){ssize_t n=write(fd,p,size);if(n<0&&errno==EINTR)continue;if(n<=0)return 0;p+=n;size-=n;}return 1;
}
int main(int argc,char **argv){
    unsigned requests=argc==2?(unsigned)strtoul(argv[1],0,10):3;if(!requests||requests>16)return 2;
    int server=socket(AF_INET,SOCK_STREAM,0);if(server<0){perror("socket");return 1;}
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(8080),.sin_addr.s_addr=htonl(INADDR_ANY)};
    if(bind(server,(struct sockaddr *)&address,sizeof(address))){perror("bind");return 2;}
    if(listen(server,8)){perror("listen");return 2;}
    puts("APT_REPO_SERVER_READY");fflush(stdout);
    const char *paths[]={"/dists/stable/main/binary-musl/Packages","/dists/stable/Release","/pool/aurora-apt-probe_1.0-1_musl-linux-amd64.deb"};
    const char *curl_paths[]={"/P","/R","/D"};
    for(unsigned request=0;request<requests;request++){
        int client=accept(server,0,0);if(client<0){perror("accept");return 3;}
        char line[1024];size_t used=0;while(used+1<sizeof(line)){
            char c;ssize_t n=read(client,&c,1);if(n!=1){perror("read request");return 4;}line[used++]=c;
            if(used>=4&&!memcmp(line+used-4,"\r\n\r\n",4))break;
        }
        line[used]=0;char path[512];
        const char *requested=request<3||request>=6?paths[request%3]:curl_paths[(request-3)%3];
        if(sscanf(line,"GET %511s HTTP/1.",path)!=1||strstr(path,"..")||strcmp(path,requested)){close(client);return 5;}
        const char *source_path=paths[request%3];
        char filename[640];int length=snprintf(filename,sizeof(filename),"/work/apt-repo%s",source_path);
        if(length<0||(size_t)length>=sizeof(filename)){close(client);return 6;}
        int file=open(filename,O_RDONLY);if(file<0){perror("open repository entry");close(client);return 7;}
        struct stat info;if(fstat(file,&info)||info.st_size<0){close(file);close(client);return 8;}
        char header[256];length=snprintf(header,sizeof(header),"HTTP/1.1 200 OK\r\nContent-Length: %lld\r\nConnection: close\r\nContent-Type: application/octet-stream\r\n\r\n",(long long)info.st_size);
        if(length<0||(size_t)length>=sizeof(header)||!send_all(client,header,(size_t)length)){close(file);close(client);return 9;}
        char buffer[1024];ssize_t n;while((n=read(file,buffer,sizeof(buffer)))>0)if(!send_all(client,buffer,(size_t)n)){close(file);close(client);return 10;}
        if(n<0||close(file)||shutdown(client,SHUT_WR)||close(client))return 11;
        printf("APT_REPO_SERVED %s\n",source_path);fflush(stdout);
    }
    close(server);puts("APT_REPO_SERVER_DONE");return 0;
}
