#pragma once
#include <stdint.h>
#include <stddef.h>
/* Raw lwIP adapter: all calls execute under the native state lock. */
void network_init(const unsigned char mac[6]);
void network_input(const void *packet,unsigned length);
void network_tick(void);
int network_socket(int type,int protocol);
void network_close(int socket);
int network_connect(int socket,const void *address,unsigned length);
int network_bind(int socket,const void *address,unsigned length);
long network_send(int socket,const void *data,size_t size,unsigned flags,const void *address,unsigned length);
long network_recv(int socket,void *data,size_t size,unsigned flags,void *address,unsigned *length);
unsigned network_readiness(int socket);
int network_name(int socket,void *address,unsigned *length,int peer);
int network_option(int socket,int level,int option,void *value,unsigned *length,int set);
int network_shutdown(int socket,int how);
unsigned network_available(int socket);
int network_configured(void);
uint32_t aurora_net_now(void);
uint32_t aurora_net_random(void);
int aurora_net_transmit(const void *packet,unsigned length);
void aurora_net_panic(const char *message);
