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
/* Grants are supervisor state. Image metadata cannot set these bits. */
enum {
    SERVICE_CAP_PCI=1ULL<<0, SERVICE_CAP_GPU=1ULL<<1,
    SERVICE_CAP_NET=1ULL<<2, SERVICE_CAP_STORAGE=1ULL<<3,
    SERVICE_CAP_ENTROPY=1ULL<<4
};
static inline u64 service_capability_for(u32 service,u32 opcode) {
    switch(service) {
    case SERVICE_PCI:return opcode>=PCI_ENUMERATE&&opcode<=PCI_BIND_IRQ?SERVICE_CAP_PCI:0;
    case SERVICE_GPU:return opcode>=GPU_PRESENT&&opcode<=GPU_QUERY?SERVICE_CAP_GPU:0;
    case SERVICE_NET:return opcode>=NET_RX&&opcode<=NET_SOCKET?SERVICE_CAP_NET:0;
    case SERVICE_STORAGE:return opcode>=STORAGE_READ&&opcode<=STORAGE_SYNC?SERVICE_CAP_STORAGE:0;
    case SERVICE_ENTROPY:return opcode==ENTROPY_READ?SERVICE_CAP_ENTROPY:0;
    default:return 0;
    }
}
static inline int service_request_allowed(u64 grants,const ServiceRequest *request) {
    u64 capability=service_capability_for(request?request->service:0,request?request->opcode:0);
    return request && capability && (grants&capability)!=0;
}
#endif
