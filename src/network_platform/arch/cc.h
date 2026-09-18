#pragma once
#include <stdint.h>
#include <stddef.h>
#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_NO_INTTYPES_H 1
#define LWIP_PLATFORM_DIAG(x) do {} while(0)
void aurora_net_panic(const char *);
uint32_t aurora_net_random(void);
#define LWIP_PLATFORM_ASSERT(x) aurora_net_panic(x)
#define LWIP_RAND() aurora_net_random()
#define PACK_STRUCT_STRUCT __attribute__((packed))
