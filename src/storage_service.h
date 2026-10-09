#ifndef AURORA_STORAGE_SERVICE_H
#define AURORA_STORAGE_SERVICE_H

/* Versioned storage request/response boundary. This is a bounded kernel broker
 * today: the block driver still owns the device, while callers exercise the
 * same validation and lifecycle contract a future ring-3 service will use. */
#define STORAGE_IPC_VERSION 1U
#define STORAGE_IPC_MAX_SECTORS 1024U
#define STORAGE_IPC_SECTOR_SIZE 512U
#define STORAGE_IPC_MAX_BYTES (STORAGE_IPC_MAX_SECTORS * STORAGE_IPC_SECTOR_SIZE)

enum { STORAGE_IPC_READ = 1, STORAGE_IPC_WRITE, STORAGE_IPC_FLUSH };
enum {
    STORAGE_IPC_OK = 0, STORAGE_IPC_E_VERSION = -1, STORAGE_IPC_E_SIZE = -2,
    STORAGE_IPC_E_OPCODE = -3, STORAGE_IPC_E_BOUNDS = -4,
    STORAGE_IPC_E_CAPABILITY = -5, STORAGE_IPC_E_DEVICE = -6,
    STORAGE_IPC_E_DMA = -7, STORAGE_IPC_E_BUSY = -8, STORAGE_IPC_E_SEQUENCE = -9,
    STORAGE_IPC_E_CANCELLED = -10, STORAGE_IPC_E_STOPPED = -11,
    STORAGE_IPC_E_IO = -12
};

typedef struct __attribute__((packed)) {
    u16 version, size;
    u32 opcode, flags, sequence, owner, device, dma_domain;
    u64 sector, count, buffer;
} StorageIpcRequest;

typedef struct __attribute__((packed)) {
    u16 version, size;
    int status;
    u32 sequence;
    u64 bytes;
} StorageIpcResponse;

typedef int (*StorageIpcDmaCheck)(u64 address, u64 length, u32 permissions);

typedef struct {
    u32 owner, device, dma_domain, next_sequence, active_sequence, generation;
    u64 sectors, capability;
    u8 active, stopped, accepting;
} StorageIpcBroker;

typedef struct {
    StorageIpcBroker *broker;
    u32 generation, owner;
    u64 capability;
    u8 active;
} StorageIpcService;

typedef int (*StorageIpcDmaRevoke)(u32 domain);

static inline int storage_ipc_request_validate(const StorageIpcRequest *request,
                                                u64 granted_capability, u64 required_capability,
                                                u32 expected_owner, u32 expected_device,
                                                u32 expected_dma_domain,
                                                u64 device_sectors,
                                                StorageIpcDmaCheck dma_check) {
    u64 bytes, end;
    if (!request) return STORAGE_IPC_E_SIZE;
    if (request->version != STORAGE_IPC_VERSION) return STORAGE_IPC_E_VERSION;
    if (request->size != sizeof(*request)) return STORAGE_IPC_E_SIZE;
    if (request->flags || request->sequence == 0) return STORAGE_IPC_E_SIZE;
    if (request->owner != expected_owner || !(granted_capability & required_capability))
        return STORAGE_IPC_E_CAPABILITY;
    if (request->device != expected_device || request->dma_domain != expected_dma_domain)
        return STORAGE_IPC_E_DEVICE;
    if (request->opcode < STORAGE_IPC_READ || request->opcode > STORAGE_IPC_FLUSH)
        return STORAGE_IPC_E_OPCODE;
    if (request->opcode == STORAGE_IPC_FLUSH) {
        if (request->sector >= device_sectors || request->count || request->buffer)
            return STORAGE_IPC_E_BOUNDS;
        return STORAGE_IPC_OK;
    }
    if (!request->count || request->count > STORAGE_IPC_MAX_SECTORS ||
        request->sector >= device_sectors || request->count > device_sectors - request->sector)
        return STORAGE_IPC_E_BOUNDS;
    if (request->count > (~0ULL / STORAGE_IPC_SECTOR_SIZE)) return STORAGE_IPC_E_BOUNDS;
    bytes = request->count * STORAGE_IPC_SECTOR_SIZE;
    if (!request->buffer || request->buffer > ~0ULL - bytes ||
        (request->buffer & (STORAGE_IPC_SECTOR_SIZE - 1))) return STORAGE_IPC_E_BOUNDS;
    end = request->buffer + bytes;
    if (end <= request->buffer || !dma_check || !dma_check(request->buffer, bytes,
                                                              request->opcode == STORAGE_IPC_READ ? 2U : 1U))
        return STORAGE_IPC_E_DMA;
    return STORAGE_IPC_OK;
}

static inline void storage_ipc_broker_init(StorageIpcBroker *broker, u32 owner,
                                           u32 device, u32 dma_domain, u64 sectors, u64 capability) {
    *broker = (StorageIpcBroker){.owner = owner, .device = device, .dma_domain = dma_domain,
                                 .generation = 1, .sectors = sectors, .capability = capability,
                                 .accepting = 1};
}

static inline int storage_ipc_submit(StorageIpcBroker *broker, const StorageIpcRequest *request,
                                     u64 granted_capability, StorageIpcDmaCheck dma_check) {
    int status;
    if (!broker || broker->stopped) return STORAGE_IPC_E_STOPPED;
    if (!broker->accepting) return STORAGE_IPC_E_STOPPED;
    if (broker->active) return STORAGE_IPC_E_BUSY;
    status = storage_ipc_request_validate(request, granted_capability, broker->capability, broker->owner,
                                           broker->device, broker->dma_domain, broker->sectors, dma_check);
    if (status) return status;
    broker->active = 1; broker->active_sequence = request->sequence;
    return STORAGE_IPC_OK;
}

static inline int storage_ipc_complete_generation(StorageIpcBroker *broker, u32 generation,
                                                  u32 sequence, int io_status, u64 bytes,
                                                  StorageIpcResponse *response) {
    if (!broker || broker->stopped) return STORAGE_IPC_E_STOPPED;
    if (generation != broker->generation) return STORAGE_IPC_E_SEQUENCE;
    if (!broker->active || sequence != broker->active_sequence) return STORAGE_IPC_E_SEQUENCE;
    if (bytes > STORAGE_IPC_MAX_BYTES) { broker->active = 0; return STORAGE_IPC_E_BOUNDS; }
    if (response) *response = (StorageIpcResponse){STORAGE_IPC_VERSION, sizeof(*response),
                                                    io_status, sequence, bytes};
    broker->active = 0; broker->active_sequence = 0;
    return io_status;
}

static inline int storage_ipc_complete(StorageIpcBroker *broker, u32 sequence,
                                       int io_status, u64 bytes, StorageIpcResponse *response) {
    return storage_ipc_complete_generation(broker, broker ? broker->generation : 0, sequence,
                                           io_status, bytes, response);
}

static inline int storage_ipc_cancel(StorageIpcBroker *broker, u32 sequence) {
    if (!broker || broker->stopped) return STORAGE_IPC_E_STOPPED;
    if (!broker->active || sequence != broker->active_sequence) return STORAGE_IPC_E_SEQUENCE;
    broker->active = 0; broker->active_sequence = 0; return STORAGE_IPC_E_CANCELLED;
}

static inline int storage_ipc_teardown(StorageIpcBroker *broker) {
    if (!broker || broker->stopped) return STORAGE_IPC_E_STOPPED;
    broker->active = 0; broker->active_sequence = 0; broker->accepting = 0; broker->stopped = 1;
    return STORAGE_IPC_OK;
}

/* Stop admission before a restart. Any single-flight request is cancelled so
 * no completion from the retiring service can become a new service response. */
static inline int storage_ipc_quiesce(StorageIpcBroker *broker) {
    if (!broker || broker->stopped) return STORAGE_IPC_E_STOPPED;
    broker->accepting = 0;
    if (broker->active) {
        broker->active = 0;
        broker->active_sequence = 0;
        return STORAGE_IPC_E_CANCELLED;
    }
    return STORAGE_IPC_OK;
}

static inline int storage_ipc_handoff(StorageIpcBroker *broker, u32 generation,
                                      u32 owner, u32 device, u32 dma_domain, u64 sectors,
                                      u64 capability, StorageIpcDmaRevoke revoke) {
    if (!broker || broker->stopped) return STORAGE_IPC_E_STOPPED;
    if (broker->active || broker->accepting) return STORAGE_IPC_E_BUSY;
    if (!generation || generation <= broker->generation || !owner || !device || !dma_domain ||
        !sectors || !capability || !revoke) return STORAGE_IPC_E_CAPABILITY;
    if (!revoke(broker->dma_domain)) return STORAGE_IPC_E_DMA;
    broker->owner = owner; broker->device = device; broker->dma_domain = dma_domain;
    broker->sectors = sectors; broker->capability = capability; broker->generation = generation;
    broker->accepting = 1;
    return STORAGE_IPC_OK;
}

static inline int storage_ipc_service_start(StorageIpcService *service, StorageIpcBroker *broker,
                                            u32 generation, u32 owner, u64 capability) {
    if (!service || !broker || broker->stopped || !broker->accepting ||
        generation != broker->generation || owner != broker->owner ||
        !(capability & broker->capability)) return STORAGE_IPC_E_CAPABILITY;
    *service = (StorageIpcService){broker, generation, owner, capability, 1};
    return STORAGE_IPC_OK;
}

static inline int storage_ipc_service_dispatch(StorageIpcService *service,
                                                const StorageIpcRequest *request,
                                                StorageIpcDmaCheck dma_check) {
    if (!service || !service->active) return STORAGE_IPC_E_STOPPED;
    return storage_ipc_submit(service->broker, request, service->capability, dma_check);
}

static inline int storage_ipc_service_complete(StorageIpcService *service, u32 sequence,
                                               int io_status, u64 bytes,
                                               StorageIpcResponse *response) {
    if (!service || !service->active) return STORAGE_IPC_E_STOPPED;
    return storage_ipc_complete_generation(service->broker, service->generation, sequence,
                                           io_status, bytes, response);
}

static inline int storage_ipc_service_exit(StorageIpcService *service) {
    if (!service || !service->active) return STORAGE_IPC_E_STOPPED;
    service->active = 0;
    return storage_ipc_quiesce(service->broker);
}

#endif
