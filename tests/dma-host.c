#include <assert.h>
#include <stdio.h>
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;
typedef unsigned long long u64; typedef long long i64;
#include "../src/dma.h"

int main(void) {
    assert(dma_init(DMA_BACKEND_SOFTWARE));
    u32 domain = dma_domain_create(7, 1, 0x100000, 0x110000);
    assert(domain && dma_assign_device(3, domain));
    assert(!dma_assign_device(3, domain));
    assert(!dma_map(domain, 0x100001, 0x1000, DMA_READ));
    assert(!dma_map(domain, 0x100000, 0x1001, DMA_READ));
    assert(!dma_map(domain, ~0xffffULL, 0x20000, DMA_READ));
    u32 mapping = dma_map(domain, 0x100000, 0x2000, DMA_READ | DMA_WRITE);
    assert(mapping && dma_validate(3, domain, 0x100000, 0x1000, DMA_READ));
    assert(dma_validate(3, domain, 0x101000, 0x1000, DMA_WRITE));
    assert(!dma_validate(3, domain, 0x102000, 0x1000, DMA_READ));
    assert(!dma_validate(4, domain, 0x100000, 0x1000, DMA_READ));
    assert(!dma_map(domain, 0x100000, 0x1000, DMA_READ));
    assert(dma_unmap(domain, mapping));
    assert(!dma_validate(3, domain, 0x100000, 0x1000, DMA_READ));
    mapping = dma_map(domain, 0x100000, 0x1000, DMA_READ);
    assert(mapping); assert(dma_revoke(domain));
    assert(!dma_validate(3, domain, 0x100000, 0x1000, DMA_READ));
    assert(dma_teardown(domain)); assert(!dma_domain(domain));
    assert(!dma_init(DMA_BACKEND_HARDWARE));
    assert(!dma_domain_create(1, 1, 0x100000, 0x110000));
    assert(dma_init(DMA_BACKEND_SOFTWARE));
    domain = dma_domain_create(7, 1, 0x100000, 0x110000);
    assert(domain && dma_assign_device(3, domain));
    assert(dma_fault_latch(1, 3, 0x0c000000, 7));
    assert(dma_faulted() && dma_fault_count() == 1 && dma_fault_source_id() == 3);
    assert(!dma_fault_latch(1, 3, 0x0c000000, 7) && dma_fault_count() == 1);
    assert(!dma_validate(3, domain, 0x100000, 0x1000, DMA_READ));
    puts("PASS DMA domain ownership, bounds, lifecycle and fail-closed backend");
    return 0;
}
