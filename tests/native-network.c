#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/random.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#define CHECK(x) do { if(!(x)){printf("FAIL network line %d: %s errno=%d\n",__LINE__,#x,errno);return 1;} } while(0)
static struct sockaddr_in host(unsigned port){struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(port)};inet_pton(AF_INET,"10.0.2.2",&a.sin_addr);return a;}
int main(void){
    unsigned char a[32],b[32];CHECK(getrandom(a,sizeof a,0)==sizeof a);CHECK(getrandom(b,sizeof b,0)==sizeof b);CHECK(memcmp(a,b,sizeof a));
    puts("PASS hardware entropy");
    int e=eventfd(0,EFD_NONBLOCK),copy=dup(e);CHECK(e>=0&&copy>=0);uint64_t v=7,r=0;CHECK(read(e,&r,8)==-1&&errno==EAGAIN);
    CHECK(write(copy,&v,8)==8);struct pollfd p={e,POLLIN,0};CHECK(poll(&p,1,0)==1&&(p.revents&POLLIN));CHECK(read(e,&r,8)==8&&r==7);
    v=UINT64_MAX;CHECK(write(e,&v,8)==-1&&errno==EINVAL);close(e);close(copy);puts("PASS eventfd shared counters and readiness");
    struct addrinfo hints={.ai_family=AF_INET,.ai_socktype=SOCK_STREAM},*answer=0;
    CHECK(getaddrinfo("example.com","443",&hints,&answer)==0);CHECK(answer&&answer->ai_family==AF_INET);freeaddrinfo(answer);
    CHECK(getaddrinfo("aurora-test.invalid","80",&hints,&answer)!=0);puts("PASS DNS resolution and NXDOMAIN");
    int s=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0);CHECK(s>=0);struct sockaddr_in dest=host(8882),from; socklen_t length=sizeof from;
    CHECK(sendto(s,"abcdef",6,0,(void*)&dest,sizeof dest)==6);p=(struct pollfd){s,POLLIN,0};CHECK(poll(&p,1,5000)==1);
    char data[64]={0};CHECK(recvfrom(s,data,2,MSG_PEEK|MSG_TRUNC,(void*)&from,&length)==6&&length==sizeof from&&from.sin_port==dest.sin_port);
    struct iovec iov[2]={{data,2},{data+2,4}};struct msghdr message={.msg_iov=iov,.msg_iovlen=2};CHECK(recvmsg(s,&message,0)==6&&!memcmp(data,"abcdef",6));
    CHECK(recv(s,data,sizeof data,0)==-1&&errno==EAGAIN);CHECK(sendto(s,(void*)1,1,0,(void*)&dest,sizeof dest)==-1&&errno==EFAULT);
    close(s);puts("PASS UDP datagrams peek truncation iovecs and pointer checks");
    s=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0);CHECK(s>=0);dest=host(8883);CHECK(connect(s,(void*)&dest,sizeof dest)==-1&&errno==EINPROGRESS);
    p=(struct pollfd){s,POLLOUT,0};CHECK(poll(&p,1,5000)==1);int error=-1;length=sizeof error;CHECK(getsockopt(s,SOL_SOCKET,SO_ERROR,&error,&length)==0&&!error);
    CHECK(fcntl(s,F_SETFL,0)==0);copy=dup(s);CHECK(copy>=0);close(s);s=copy;
    CHECK(send(s,"abcdef",6,MSG_NOSIGNAL)==6);CHECK(recv(s,data,2,MSG_PEEK)==2&&!memcmp(data,"ab",2));
    unsigned total=0;while(total<6){int n=read(s,data+total,6-total);CHECK(n>0);total+=n;}CHECK(!memcmp(data,"abcdef",6));
    CHECK(shutdown(s,SHUT_WR)==0);CHECK(read(s,data,1)==0);close(s);puts("PASS TCP nonblocking connect dup partial reads and half close");
    for(int i=0;i<40;i++){s=socket(AF_INET,SOCK_STREAM,0);CHECK(s>=0);dest=host(8883);CHECK(connect(s,(void*)&dest,sizeof dest)==0);CHECK(shutdown(s,SHUT_RDWR)==0);close(s);}
    puts("PASS repeated TCP shutdown and descriptor reuse");
    s=socket(AF_INET,SOCK_STREAM,0);CHECK(s>=0);dest=host(8899);CHECK(connect(s,(void*)&dest,sizeof dest)==-1&&errno==ECONNREFUSED);close(s);
    puts("PASS refused TCP connection");
    puts("AURORA_NETWORK_ABI_PASS");return 0;
}
