#pragma once
#define LWIP_HOOK_TCP_ISN(local_ip,local_port,remote_ip,remote_port) aurora_net_random()
