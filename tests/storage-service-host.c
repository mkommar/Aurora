#include <assert.h>
#include <limits.h>
#include <stdio.h>
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;
typedef unsigned long long u64; typedef long long i64;
#include "../src/storage_service.h"

static u64 allowed_base, allowed_size;
static u32 revoked_domain;
static int dma_check(u64 address, u64 length, u32 permissions) {
    return (permissions == 1 || permissions == 2) && !(length & 4095) && address >= allowed_base &&
           length <= allowed_size && address + length > address && address + length <= allowed_base + allowed_size;
}
static int dma_revoke(u32 domain) { revoked_domain = domain; return 1; }
static StorageIpcRequest request(u32 opcode, u32 sequence) {
    return (StorageIpcRequest){.version = STORAGE_IPC_VERSION, .size = sizeof(StorageIpcRequest),
                               .opcode = opcode, .sequence = sequence, .owner = 4,
                               .device = 9, .dma_domain = 2, .sector = 20,
                               .count = 20, .buffer = 0x200000};
}

int main(void) {
    StorageIpcBroker broker; StorageIpcService service, replacement; StorageIpcResponse response;
    allowed_base = 0x200000; allowed_size = STORAGE_IPC_MAX_BYTES;
    storage_ipc_broker_init(&broker, 4, 9, 2, 1000, 1ULL << 3);
    StorageIpcRequest read = request(STORAGE_IPC_READ, 1);
    assert(storage_ipc_service_start(&service, &broker, 2, 4, 1ULL << 3) == STORAGE_IPC_E_CAPABILITY);
    assert(storage_ipc_service_start(&service, &broker, 1, 5, 1ULL << 3) == STORAGE_IPC_E_CAPABILITY);
    assert(storage_ipc_service_start(&service, &broker, 1, 4, 0) == STORAGE_IPC_E_CAPABILITY);
    assert(storage_ipc_service_start(&service, &broker, 1, 4, 1ULL << 3) == STORAGE_IPC_OK);
    assert(storage_ipc_service_dispatch(&service, &read, dma_check) == STORAGE_IPC_OK);
    assert(storage_ipc_handoff(&broker, 2, 5, 10, 3, 1000, 1ULL << 3, dma_revoke) == STORAGE_IPC_E_BUSY);
    assert(storage_ipc_submit(&broker, &read, 1ULL << 3, dma_check) == STORAGE_IPC_E_BUSY);
    assert(storage_ipc_complete(&broker, 99, STORAGE_IPC_OK, 512, &response) == STORAGE_IPC_E_SEQUENCE);
    assert(storage_ipc_complete(&broker, 1, STORAGE_IPC_OK, 512, &response) == STORAGE_IPC_OK);
    assert(response.sequence == 1 && response.bytes == 512);
    read.version = 2; assert(storage_ipc_submit(&broker, &read, 1ULL << 3, dma_check) == STORAGE_IPC_E_VERSION); read.version = 1;
    read.size--; assert(storage_ipc_submit(&broker, &read, 1ULL << 3, dma_check) == STORAGE_IPC_E_SIZE); read.size = sizeof(read);
    read.opcode = 99; assert(storage_ipc_submit(&broker, &read, 1ULL << 3, dma_check) == STORAGE_IPC_E_OPCODE); read.opcode = STORAGE_IPC_READ;
    read.count = STORAGE_IPC_MAX_SECTORS + 1; assert(storage_ipc_submit(&broker, &read, 1ULL << 3, dma_check) == STORAGE_IPC_E_BOUNDS); read.count = 20;
    read.sector = ULLONG_MAX; assert(storage_ipc_submit(&broker, &read, 1ULL << 3, dma_check) == STORAGE_IPC_E_BOUNDS); read.sector = 20;
    read.owner = 5; assert(storage_ipc_submit(&broker, &read, 1ULL << 3, dma_check) == STORAGE_IPC_E_CAPABILITY); read.owner = 4;
    read.device = 10; assert(storage_ipc_submit(&broker, &read, 1ULL << 3, dma_check) == STORAGE_IPC_E_DEVICE); read.device = 9;
    read.dma_domain = 3; assert(storage_ipc_submit(&broker, &read, 1ULL << 3, dma_check) == STORAGE_IPC_E_DEVICE); read.dma_domain = 2;
    read.buffer = 0x200001; assert(storage_ipc_submit(&broker, &read, 1ULL << 3, dma_check) == STORAGE_IPC_E_BOUNDS); read.buffer = 0x200000;
    read.buffer = 0x300000; assert(storage_ipc_submit(&broker, &read, 1ULL << 3, dma_check) == STORAGE_IPC_E_DMA); read.buffer = 0x200000;
    assert(storage_ipc_submit(&broker, &read, 0, dma_check) == STORAGE_IPC_E_CAPABILITY);
    assert(storage_ipc_service_dispatch(&service, &read, dma_check) == STORAGE_IPC_OK);
    assert(storage_ipc_complete(&broker, 1, STORAGE_IPC_E_IO, 0, &response) == STORAGE_IPC_E_IO);
    assert(response.status == STORAGE_IPC_E_IO && response.bytes == 0);
    assert(storage_ipc_service_dispatch(&service, &read, dma_check) == STORAGE_IPC_OK);
    assert(storage_ipc_service_exit(&service) == STORAGE_IPC_E_CANCELLED);
    assert(storage_ipc_cancel(&broker, 1) == STORAGE_IPC_E_SEQUENCE);
    read.opcode = STORAGE_IPC_FLUSH; read.count = 0; read.buffer = 0;
    assert(storage_ipc_complete_generation(&broker, 1, 1, STORAGE_IPC_OK, 0, &response) == STORAGE_IPC_E_SEQUENCE);
    read.owner = 5; read.device = 10; read.dma_domain = 3;
    assert(storage_ipc_handoff(&broker, 2, 5, 10, 3, 1000, 1ULL << 3, dma_revoke) == STORAGE_IPC_OK);
    assert(revoked_domain == 2);
    assert(storage_ipc_service_start(&replacement, &broker, 2, 5, 1ULL << 3) == STORAGE_IPC_OK);
    assert(storage_ipc_service_dispatch(&replacement, &read, dma_check) == STORAGE_IPC_OK);
    assert(storage_ipc_service_dispatch(&replacement, &read, dma_check) == STORAGE_IPC_E_BUSY);
    assert(storage_ipc_service_complete(&service, 1, STORAGE_IPC_OK, 0, &response) == STORAGE_IPC_E_STOPPED);
    assert(storage_ipc_complete_generation(&broker, 1, 1, STORAGE_IPC_OK, 0, &response) == STORAGE_IPC_E_SEQUENCE);
    assert(storage_ipc_service_complete(&replacement, 1, STORAGE_IPC_OK, 512, &response) == STORAGE_IPC_OK);
    read.opcode = STORAGE_IPC_FLUSH; read.sector = 20; read.count = 0; read.buffer = 0;
    assert(storage_ipc_service_dispatch(&replacement, &read, dma_check) == STORAGE_IPC_OK);
    assert(storage_ipc_teardown(&broker) == STORAGE_IPC_OK);
    assert(storage_ipc_complete(&broker, 1, STORAGE_IPC_OK, 0, &response) == STORAGE_IPC_E_STOPPED);
    puts("PASS storage IPC ABI validation, ownership, DMA, lifecycle and cancellation");
    return 0;
}
