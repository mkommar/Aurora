#ifndef AURORA_SERVICE_ABI_H
#define AURORA_SERVICE_ABI_H
#include "abi.h"
/* Capability-scoped service endpoints. Rings are mapped only to the owner and
 * the kernel broker; device addresses never cross this ABI. */
enum { SERVICE_PCI=1, SERVICE_GPU, SERVICE_NET, SERVICE_STORAGE, SERVICE_ENTROPY };
enum { SERVICE_RING_EMPTY, SERVICE_RING_READY, SERVICE_RING_DONE, SERVICE_RING_ERROR };
typedef struct __attribute__((packed)) {
    u32 service,opcode,status,sequence;
    u64 argument[4];
} ServiceRequest;
typedef struct __attribute__((aligned(64))) {
    volatile u32 producer,consumer;
    ServiceRequest request[32];
} ServiceRing;
enum {
    PCI_ENUMERATE=1, PCI_MAP_BAR, PCI_BIND_IRQ,
    GPU_PRESENT=16, GPU_FLUSH, GPU_QUERY,
    NET_RX=32, NET_TX, NET_SOCKET,
    STORAGE_READ=48, STORAGE_WRITE, STORAGE_SYNC,
    ENTROPY_READ=64
};
#endif
