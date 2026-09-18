/* lwIP owns packet parsing, retransmission, ARP and DHCP. Aurora owns handles,
 * descriptor lifetime and wait queues. This module never enters the scheduler. */
#include "network_api.h"
#include <string.h>
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#define NET_SOCKETS 32
#define UDP_PENDING 8
typedef struct {uint16_t family,port;uint32_t ip;unsigned char zero[8];} NetAddress;
typedef struct {
    int type,connecting,connected,error,eof,read_closed,write_closed;
    struct tcp_pcb *tcp;struct udp_pcb *udp;struct pbuf *received;
    struct pbuf *datagrams[UDP_PENDING];NetAddress senders[UDP_PENDING];unsigned head,count;
} NetSocket;
static NetSocket sockets[NET_SOCKETS];
static struct netif interface;
static int initialized;
volatile uint64_t network_rx_packets,network_tx_packets,network_rx_drops,network_connections;
uint32_t sys_now(void){return aurora_net_now();}
static int error_number(err_t error){
    switch(error){case ERR_OK:return 0;case ERR_MEM:case ERR_BUF:return 11;case ERR_TIMEOUT:return 110;
    case ERR_RTE:return 101;case ERR_INPROGRESS:return 115;case ERR_WOULDBLOCK:return 11;case ERR_USE:return 98;
    case ERR_ALREADY:return 114;case ERR_ISCONN:return 106;case ERR_CONN:return 107;case ERR_ABRT:return 103;
    case ERR_RST:return 104;case ERR_CLSD:return 32;default:return 22;}
}
static NetSocket *get_socket(int handle){return handle>=0&&handle<NET_SOCKETS&&sockets[handle].type?&sockets[handle]:0;}
static int parse_address(const void *address,unsigned length,ip_addr_t *ip,uint16_t *port){
    if(!address||length<sizeof(NetAddress))return -22;const NetAddress *a=address;if(a->family!=2)return -97;
    ip_addr_set_ip4_u32(ip,a->ip);*port=lwip_ntohs(a->port);return 0;
}
static void copy_address(void *out,unsigned *length,const NetAddress *a){
    if(!out||!length)return;unsigned n=*length<sizeof(*a)?*length:sizeof(*a);memcpy(out,a,n);*length=sizeof(*a);
}
static err_t transmit(struct netif *n,struct pbuf *p){
    (void)n;unsigned char packet[1518];if(p->tot_len>sizeof(packet))return ERR_BUF;
    pbuf_copy_partial(p,packet,p->tot_len,0);if(!aurora_net_transmit(packet,p->tot_len))return ERR_MEM;
    network_tx_packets++;return ERR_OK;
}
static err_t interface_init(struct netif *n){
    n->name[0]='e';n->name[1]='n';n->hostname="aurora";n->mtu=1500;
    n->flags=NETIF_FLAG_BROADCAST|NETIF_FLAG_ETHARP|NETIF_FLAG_LINK_UP;
    n->output=etharp_output;n->linkoutput=transmit;return ERR_OK;
}
void network_init(const unsigned char mac[6]){
    lwip_init();ip4_addr_t zero={0};
    if(!netif_add(&interface,&zero,&zero,&zero,0,interface_init,ethernet_input))return;
    memcpy(interface.hwaddr,mac,6);interface.hwaddr_len=6;netif_set_default(&interface);netif_set_up(&interface);
    initialized=1;if(dhcp_start(&interface)!=ERR_OK)aurora_net_panic("DHCP initialization");
}
int network_configured(void){return initialized&&!ip4_addr_isany_val(*netif_ip4_addr(&interface));}
void network_input(const void *packet,unsigned length){
    if(!initialized||length<14||length>1518){network_rx_drops++;return;}
    struct pbuf *p=pbuf_alloc(PBUF_RAW,(uint16_t)length,PBUF_RAM);if(!p){network_rx_drops++;return;}
    pbuf_take(p,packet,(uint16_t)length);network_rx_packets++;
    if(interface.input(p,&interface)!=ERR_OK){pbuf_free(p);network_rx_drops++;}
}
void network_tick(void){if(initialized)sys_check_timeouts();}
static void tcp_error(void *argument,err_t error){NetSocket *s=argument;s->tcp=0;s->error=s->connecting&&error==ERR_RST?111:error_number(error);s->connecting=0;s->eof=1;}
static err_t connected(void *argument,struct tcp_pcb *pcb,err_t error){
    NetSocket *s=argument;(void)pcb;s->connecting=0;s->error=error_number(error);s->connected=error==ERR_OK;
    if(s->connected)network_connections++;return ERR_OK;
}
static err_t receive_tcp(void *argument,struct tcp_pcb *pcb,struct pbuf *p,err_t error){
    NetSocket *s=argument;(void)pcb;
    if(!p){s->eof=1;return ERR_OK;}if(error!=ERR_OK)return error;
    if(s->read_closed){tcp_recved(pcb,p->tot_len);pbuf_free(p);return ERR_OK;}
    if(s->received)pbuf_cat(s->received,p);else s->received=p;return ERR_OK;
}
static void receive_udp(void *argument,struct udp_pcb *pcb,struct pbuf *p,const ip_addr_t *ip,uint16_t port){
    NetSocket *s=argument;(void)pcb;if(!p)return;
    if(s->count==UDP_PENDING||s->read_closed){pbuf_free(p);network_rx_drops++;return;}
    unsigned tail=(s->head+s->count)%UDP_PENDING;s->datagrams[tail]=p;
    s->senders[tail]=(NetAddress){.family=2,.port=lwip_htons(port),.ip=ip_addr_get_ip4_u32(ip)};s->count++;
}
int network_socket(int type,int protocol){
    if(!initialized)return -100;
    if((type!=1&&type!=2)||(protocol&&protocol!=(type==1?6:17)))return -93;
    int handle;for(handle=0;handle<NET_SOCKETS&&sockets[handle].type;handle++){}if(handle==NET_SOCKETS)return -23;
    NetSocket *s=&sockets[handle];memset(s,0,sizeof(*s));
    if(type==1){s->tcp=tcp_new_ip_type(IPADDR_TYPE_V4);if(!s->tcp)return -12;
        tcp_arg(s->tcp,s);tcp_recv(s->tcp,receive_tcp);tcp_err(s->tcp,tcp_error);
    }else{s->udp=udp_new_ip_type(IPADDR_TYPE_V4);if(!s->udp)return -12;udp_recv(s->udp,receive_udp,s);}
    s->type=type;return handle;
}
void network_close(int handle){
    NetSocket *s=get_socket(handle);if(!s)return;
    if(s->tcp){tcp_arg(s->tcp,0);tcp_recv(s->tcp,0);tcp_err(s->tcp,0);if(tcp_close(s->tcp)!=ERR_OK)tcp_abort(s->tcp);}
    if(s->udp)udp_remove(s->udp);if(s->received)pbuf_free(s->received);
    for(unsigned i=0;i<s->count;i++)pbuf_free(s->datagrams[(s->head+i)%UDP_PENDING]);memset(s,0,sizeof(*s));
}
int network_connect(int handle,const void *address,unsigned length){
    NetSocket *s=get_socket(handle);if(!s)return -9;ip_addr_t ip;uint16_t port;int r=parse_address(address,length,&ip,&port);if(r)return r;
    if(!network_configured())return -101;if(s->error)return -s->error;if(s->connected)return 0;if(s->connecting)return -115;
    err_t error;if(s->type==2){error=udp_connect(s->udp,&ip,port);if(!error)s->connected=1;}
    else{error=tcp_connect(s->tcp,&ip,port,connected);if(!error){s->connecting=1;return -115;}}
    return -error_number(error);
}
int network_bind(int handle,const void *address,unsigned length){
    NetSocket *s=get_socket(handle);if(!s)return -9;ip_addr_t ip;uint16_t port;int r=parse_address(address,length,&ip,&port);if(r)return r;
    return -error_number(s->type==1?tcp_bind(s->tcp,&ip,port):udp_bind(s->udp,&ip,port));
}
long network_send(int handle,const void *data,size_t size,unsigned flags,const void *address,unsigned length){
    NetSocket *s=get_socket(handle);if(!s)return -9;if(flags&~(0x40U|0x4000U|0x8000U))return -95;
    if(s->error)return -s->error;if(s->write_closed)return -32;if(!network_configured())return -101;
    if(s->type==1){if(!s->connected||!s->tcp)return -107;if(!size)return 0;
        size_t count=tcp_sndbuf(s->tcp);if(!count)return -11;if(size<count)count=size;
        err_t error=tcp_write(s->tcp,data,(uint16_t)count,TCP_WRITE_FLAG_COPY);if(error)return -error_number(error);
        tcp_output(s->tcp);return (long)count;
    }
    if(size>65507)return -90;ip_addr_t ip;uint16_t port=0;
    if(address){int r=parse_address(address,length,&ip,&port);if(r)return r;}else if(!s->connected)return -89;
    struct pbuf *p=pbuf_alloc(PBUF_TRANSPORT,(uint16_t)size,PBUF_RAM);if(!p)return -12;
    if(size)pbuf_take(p,data,(uint16_t)size);err_t error=address?udp_sendto(s->udp,p,&ip,port):udp_send(s->udp,p);pbuf_free(p);
    return error?-error_number(error):(long)size;
}
long network_recv(int handle,void *data,size_t size,unsigned flags,void *address,unsigned *length){
    NetSocket *s=get_socket(handle);if(!s)return -9;if(flags&~(2U|0x20U|0x40U))return -95;if(s->read_closed)return 0;
    if(s->type==1){
        if(!size)return 0;if(!s->received)return s->error?-s->error:s->eof?0:-11;
        unsigned n=s->received->tot_len;if(size<n)n=(unsigned)size;pbuf_copy_partial(s->received,data,(uint16_t)n,0);
        if(!(flags&2)){s->received=pbuf_free_header(s->received,(uint16_t)n);if(s->tcp){tcp_recved(s->tcp,(uint16_t)n);tcp_output(s->tcp);}}
        if(address)network_name(handle,address,length,1);return n;
    }
    if(!s->count)return s->error?-s->error:-11;struct pbuf *p=s->datagrams[s->head];unsigned total=p->tot_len,n=size<total?(unsigned)size:total;
    if(n)pbuf_copy_partial(p,data,(uint16_t)n,0);copy_address(address,length,&s->senders[s->head]);
    if(!(flags&2)){pbuf_free(p);s->datagrams[s->head]=0;s->head=(s->head+1)%UDP_PENDING;s->count--;}
    return flags&0x20?total:n;
}
unsigned network_readiness(int handle){
    NetSocket *s=get_socket(handle);if(!s)return 32;unsigned result=0;
    if(s->received||s->count||s->eof||s->read_closed)result|=1;
    if(s->type==2||(s->connected&&s->tcp&&tcp_sndbuf(s->tcp)&&!s->write_closed))result|=4;
    if(s->error)result|=8|4;if(s->eof)result|=16;return result;
}
unsigned network_available(int handle){NetSocket *s=get_socket(handle);return !s?0:s->type==1?(s->received?s->received->tot_len:0):(s->count?s->datagrams[s->head]->tot_len:0);}
int network_name(int handle,void *address,unsigned *length,int peer){
    NetSocket *s=get_socket(handle);if(!s)return -9;if(peer&&!s->connected)return -107;
    const ip_addr_t *ip;uint16_t port;
    if(s->type==1){if(!s->tcp)return -107;ip=peer?&s->tcp->remote_ip:&s->tcp->local_ip;port=peer?s->tcp->remote_port:s->tcp->local_port;}
    else{ip=peer?&s->udp->remote_ip:&s->udp->local_ip;port=peer?s->udp->remote_port:s->udp->local_port;}
    NetAddress a={.family=2,.port=lwip_htons(port),.ip=ip_addr_get_ip4_u32(ip)};copy_address(address,length,&a);return 0;
}
int network_option(int handle,int level,int option,void *value,unsigned *length,int set){
    NetSocket *s=get_socket(handle);if(!s)return -9;if(!length||!value)return -14;
    if(set&&*length<4)return -22;int v=set?*(int *)value:0,result=0;
    if(level==1){
        if(option==4&&!set){result=s->error;s->error=0;}
        else if(option==3&&!set)result=s->type;
        else if(option==7||option==8){if(set&&v<0)return -22;result=option==7?TCP_SND_BUF:TCP_WND;}
        else if(option==9&&s->tcp){if(set){if(v)ip_set_option(s->tcp,SOF_KEEPALIVE);else ip_reset_option(s->tcp,SOF_KEEPALIVE);}result=ip_get_option(s->tcp,SOF_KEEPALIVE)!=0;}
        else if(option==2){/* No listeners exist; client bind still rejects collisions. */result=0;}
        else return -92;
    }else if(level==6&&s->tcp){
        if(option==1){if(set){if(v)tcp_nagle_disable(s->tcp);else tcp_nagle_enable(s->tcp);}result=tcp_nagle_disabled(s->tcp)!=0;}
        else if(option>=4&&option<=6){if(set&&(v<=0||v>32767))return -22;
            if(option==4){if(set)s->tcp->keep_idle=(uint32_t)v*1000;result=s->tcp->keep_idle/1000;}
            if(option==5){if(set)s->tcp->keep_intvl=(uint32_t)v*1000;result=s->tcp->keep_intvl/1000;}
            if(option==6){if(set)s->tcp->keep_cnt=(uint32_t)v;result=s->tcp->keep_cnt;}}
        else return -92;
    }else return -92;
    if(!set){unsigned n=*length<4?*length:4;memcpy(value,&result,n);*length=4;}return 0;
}
int network_shutdown(int handle,int how){
    NetSocket *s=get_socket(handle);if(!s)return -9;if(how<0||how>2)return -22;if(!s->connected)return -107;
    if(s->tcp){
        if(how==2||(how==1&&s->read_closed)||(how==0&&s->write_closed)){
            struct tcp_pcb *pcb=s->tcp;tcp_arg(pcb,0);tcp_recv(pcb,0);tcp_err(pcb,0);
            if(tcp_close(pcb)!=ERR_OK)tcp_abort(pcb);s->tcp=0;
        }else{err_t error=tcp_shutdown(s->tcp,how!=1,how!=0);if(error)return -error_number(error);}
    }
    if(how!=1){s->read_closed=1;if(s->received){pbuf_free(s->received);s->received=0;}}
    if(how!=0)s->write_closed=1;return 0;
}
