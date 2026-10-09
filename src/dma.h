#ifndef AURORA_DMA_H
#define AURORA_DMA_H

/* Capability-scoped DMA ownership. The kernel selects the VT-d backend and
 * refuses DMA-backed devices when discovery or programming fails. */
#include "vtd.h"
#define DMA_PAGE 4096ULL
#define DMA_MAX_DOMAINS 8
#define DMA_MAX_MAPS 32
#define DMA_MAX_DEVICES 32
#define DMA_BACKEND_SOFTWARE 1
#define DMA_BACKEND_HARDWARE 2
#define DMA_READ 1
#define DMA_WRITE 2
#define DMA_DEVICE_NONE 0xffffffffU
#define DMA_UNUSED __attribute__((unused))
#define DMA_VTD_PAGES 64
#define DMA_VTD_MEMORY 0x01000000ULL
#define DMA_VTD_ROOT 0x01200000ULL

typedef struct {
    u64 address, length;
    u32 permissions, active;
} DmaMapping;

typedef struct {
    u32 id, owner, active;
    u64 capability;
    u64 policy_base, policy_end;
    DmaMapping mappings[DMA_MAX_MAPS];
    u64 second_level;
} DmaDomain;

static DmaDomain dma_domains[DMA_MAX_DOMAINS];
static u32 dma_device_key[DMA_MAX_DEVICES];
static u32 dma_device_domain[DMA_MAX_DEVICES];
static int dma_backend;
static int dma_hw_faulted;
static volatile u64 dma_faults;
static volatile u32 dma_fault_status;
#if !defined(__STDC_HOSTED__) || !__STDC_HOSTED__
static u32 dma_vtd_next_page;
static u64 dma_vtd_mmio;
static u64 dma_vtd_root;
static u64 dma_vtd_cap;
#endif

static int dma_aligned(u64 value) { return !(value & (DMA_PAGE - 1)); }
static int dma_range_valid(u64 address, u64 length, u64 *end) {
    if (!length || !dma_aligned(address) || !dma_aligned(length) ||
        address > ~0ULL - length) return 0;
    *end = address + length;
    return *end > address;
}
static int dma_contains(u64 base, u64 limit, u64 address, u64 length) {
    u64 end;
    return dma_range_valid(address, length, &end) && address >= base && end <= limit;
}

static DMA_UNUSED int dma_vtd_alloc(u64 *address) {
#if !defined(__STDC_HOSTED__) || !__STDC_HOSTED__
    if (dma_vtd_next_page >= DMA_VTD_PAGES) return 0;
    *address = DMA_VTD_MEMORY + (u64)dma_vtd_next_page++ * DMA_PAGE;
    for (u32 i = 0; i < DMA_PAGE; i++) ((u8 *)(u64)*address)[i] = 0;
    return 1;
#else
    (void)address; return 0;
#endif
}
#if !defined(__STDC_HOSTED__) || !__STDC_HOSTED__
static u64 dma_mmio_read(u32 offset) { return *(volatile u64 *)(dma_vtd_mmio + offset); }
static void dma_mmio_write(u32 offset, u64 value) { *(volatile u64 *)(dma_vtd_mmio + offset) = value; }
static int dma_wait(u32 offset, u64 mask, u64 value) {
    for (u32 i = 0; i < 1000000; i++) if ((dma_mmio_read(offset) & mask) == value) return 1;
    return 0;
}
static int dma_vtd_flush(void) {
    /* Global context and IOTLB invalidation are ordered after page-table writes. */
    __asm__ volatile("mfence" ::: "memory");
    dma_mmio_write(0x28, (1ULL << 63) | (1ULL << 61));
    if (!dma_wait(0x28, 1ULL << 63, 0)) return 0;
    u32 iotlb = (u32)((dma_vtd_cap >> 24) & 0xff) * 16;
    if (iotlb) {
        dma_mmio_write(iotlb, (1ULL << 63) | (1ULL << 60));
        if (!dma_wait(iotlb, 1ULL << 63, 0)) return 0;
    }
    __asm__ volatile("mfence" ::: "memory");
    return 1;
}
static int dma_vtd_scan_root(const u8 *root, u32 length, u32 entry_size, u64 *found) {
        if (length < 36 || length > 65536 || !vtd_checksum(root, length)) return 0;
        for (u32 offset = 36; offset + entry_size <= length; offset += entry_size) {
            u64 table_address = entry_size == 4 ? *(const u32 *)(root + offset) : *(const u64 *)(root + offset);
            if (table_address < 0x100000 || table_address > 0xffffffffULL - 48) continue;
            const u8 *table = (const u8 *)(u64)table_address;
            u32 size = *(const u32 *)(table + 4);
            VtdUnit unit;
            if (size >= 48 && size <= 65536 && table_address <= 0xffffffffULL - size &&
                !__builtin_memcmp(table, "DMAR", 4) && vtd_parse_dmar(table, size, &unit) && unit.include_all) {
                *found = unit.register_base; return 1;
            }
        }
        return 0;
}
static int dma_vtd_dmar(u64 *base) {
    u32 ranges[4] = {(*(volatile u16 *)0x40e) * 16U, 1024, 0xe0000, 0x20000};
    for (int range = 0; range < 4; range += 2)
        for (u32 address = ranges[range]; address && address < ranges[range] + ranges[range + 1]; address += 16) {
            const u8 *rsdp = (const u8 *)(u64)address;
            if (__builtin_memcmp(rsdp, "RSD PTR ", 8) || !vtd_checksum(rsdp, 20)) continue;
            u32 root = *(const u32 *)(rsdp + 16);
            if (root < 0x100000 || root > 0xfffff000U) continue;
            const u8 *rsdt = (const u8 *)(u64)root;
            u32 length = *(const u32 *)(rsdt + 4);
            if (__builtin_memcmp(rsdt, "RSDT", 4) || root > 0xffffffffU - length) continue;
            if (dma_vtd_scan_root(rsdt, length, 4, base)) return 1;
            if (rsdp[15] >= 2 && vtd_checksum(rsdp, 36)) {
                u64 xsdt_address = *(const u64 *)(rsdp + 24);
                if (xsdt_address >= 0x100000 && xsdt_address <= 0xffffffffULL - 48) {
                    const u8 *xsdt = (const u8 *)(u64)xsdt_address;
                    u32 xsdt_length = *(const u32 *)(xsdt + 4);
                    if (!__builtin_memcmp(xsdt, "XSDT", 4) && dma_vtd_scan_root(xsdt, xsdt_length, 8, base)) return 1;
                }
            }
        }
    return 0;
}
static int dma_vtd_init(void) {
    u64 base;
    if (!dma_vtd_dmar(&base)) return 0;
    dma_vtd_mmio = base; dma_vtd_cap = dma_mmio_read(8); dma_vtd_next_page = 0;
    dma_vtd_root = DMA_VTD_ROOT;
    for (u32 i = 0; i < 512; i++) ((u64 *)(u64)dma_vtd_root)[i] = 0;
    dma_mmio_write(0x20, dma_vtd_root);
    dma_mmio_write(0x18, dma_mmio_read(0x18) | (1ULL << 30));
    if (!dma_wait(0x1c, 1ULL << 30, 1ULL << 30)) return 0;
    dma_mmio_write(0x18, dma_mmio_read(0x18) | (1ULL << 31));
    if (!dma_wait(0x1c, 1ULL << 31, 1ULL << 31)) return 0;
    return 1;
}
#endif
static int dma_init(int backend) {
    for (u32 i = 0; i < DMA_MAX_DOMAINS; i++) dma_domains[i].active = 0;
    for (u32 i = 0; i < DMA_MAX_DEVICES; i++) { dma_device_key[i] = DMA_DEVICE_NONE; dma_device_domain[i] = DMA_DEVICE_NONE; }
    dma_hw_faulted = 0; dma_faults = 0;
    dma_backend = backend == DMA_BACKEND_SOFTWARE ? DMA_BACKEND_SOFTWARE : 0;
#if !defined(__STDC_HOSTED__) || !__STDC_HOSTED__
    if (backend == DMA_BACKEND_HARDWARE && dma_vtd_init()) dma_backend = DMA_BACKEND_HARDWARE;
#endif
    return dma_backend != 0;
}
static int dma_domain_create(u32 owner, u64 capability, u64 policy_base, u64 policy_end) {
    if (!dma_backend || !capability || policy_end <= policy_base ||
        !dma_aligned(policy_base) || !dma_aligned(policy_end)) return 0;
    for (u32 i = 0; i < DMA_MAX_DOMAINS; i++) if (!dma_domains[i].active) {
        u64 second_level = 0;
#if !defined(__STDC_HOSTED__) || !__STDC_HOSTED__
        if (dma_backend == DMA_BACKEND_HARDWARE && !dma_vtd_alloc(&second_level)) return 0;
#endif
        dma_domains[i] = (DmaDomain){.id = i + 1, .owner = owner, .capability = capability, .active = 1,
                                     .policy_base = policy_base, .policy_end = policy_end,
                                     .second_level = second_level};
#if !defined(__STDC_HOSTED__) || !__STDC_HOSTED__
        if (dma_backend == DMA_BACKEND_HARDWARE) {
            u64 *root = (u64 *)(u64)second_level, pdpt, pd;
            if (!dma_vtd_alloc(&pdpt) || !dma_vtd_alloc(&pd)) return 0;
            root[0] = pdpt | 3; ((u64 *)(u64)pdpt)[0] = pd | 3;
        }
#endif
        return i + 1;
    }
    return 0;
}
static DmaDomain *dma_domain(u32 id) {
    return id && id <= DMA_MAX_DOMAINS && dma_domains[id - 1].active ? &dma_domains[id - 1] : 0;
}
static int dma_assign_device(u32 device, u32 domain_id) {
    DmaDomain *domain = dma_domain(domain_id);
    if (!domain) return 0;
    for (u32 i = 0; i < DMA_MAX_DEVICES; i++) if (dma_device_key[i] == device) return 0;
    for (u32 i = 0; i < DMA_MAX_DEVICES; i++) if (dma_device_key[i] == DMA_DEVICE_NONE) {
        dma_device_key[i] = device; dma_device_domain[i] = domain_id;
#if !defined(__STDC_HOSTED__) || !__STDC_HOSTED__
        if (dma_backend == DMA_BACKEND_HARDWARE) {
            u32 bus = device >> 16, devfn = ((device >> 11) & 0x1f) << 3;
            u64 *root = (u64 *)(u64)dma_vtd_root;
            if (!root[bus * 2]) { u64 context_page; if (!dma_vtd_alloc(&context_page)) return 0; root[bus * 2] = context_page | 1; }
            u64 *context = (u64 *)(u64)(root[bus * 2] & ~0xfffULL);
            context += devfn * 2;
            context[0] = vtd_context_entry((u16)domain_id);
            context[1] = vtd_context_attributes(domain->second_level);
            if (!dma_vtd_flush()) { dma_device_key[i] = DMA_DEVICE_NONE; dma_device_domain[i] = DMA_DEVICE_NONE; return 0; }
        }
#endif
        return 1;
    }
    return 0;
}
static int dma_map(u32 domain_id, u64 address, u64 length, u32 permissions) {
    DmaDomain *domain = dma_domain(domain_id); u64 end;
    if (!domain || !(permissions & (DMA_READ | DMA_WRITE)) ||
        permissions & ~(DMA_READ | DMA_WRITE) || !dma_range_valid(address, length, &end) ||
        !dma_contains(domain->policy_base, domain->policy_end, address, length)) return 0;
    for (u32 i = 0; i < DMA_MAX_MAPS; i++) if (!domain->mappings[i].active) {
        for (u32 j = 0; j < DMA_MAX_MAPS; j++) if (domain->mappings[j].active) {
            u64 existing_end = domain->mappings[j].address + domain->mappings[j].length;
            if (address < existing_end && domain->mappings[j].address < end) return 0;
        }
#if !defined(__STDC_HOSTED__) || !__STDC_HOSTED__
        if (dma_backend == DMA_BACKEND_HARDWARE) {
            for (u64 page = address; page < end; page += DMA_PAGE) {
                u32 l1 = (u32)((page >> 39) & 511), l2 = (u32)((page >> 30) & 511), l3 = (u32)((page >> 21) & 511), l4 = (u32)((page >> 12) & 511);
                u64 *pml4 = (u64 *)(u64)domain->second_level, *pdpt, *pd, *pt;
                if (!pml4[l1] && !dma_vtd_alloc(&pml4[l1])) return 0;
                pdpt = (u64 *)(u64)(pml4[l1] & ~0xfffULL);
                if (!pdpt[l2] && !dma_vtd_alloc(&pdpt[l2])) return 0;
                pd = (u64 *)(u64)(pdpt[l2] & ~0xfffULL);
                if (!pd[l3] && !dma_vtd_alloc(&pd[l3])) return 0;
                pt = (u64 *)(u64)(pd[l3] & ~0xfffULL);
                pt[l4] = vtd_leaf_entry(page, permissions);
            }
            if (!dma_vtd_flush()) return 0;
        }
#endif
        domain->mappings[i] = (DmaMapping){address, length, permissions, 1};
        return i + 1;
    }
    return 0;
}
static int dma_device_allows(u32 device, u32 domain_id) {
    for (u32 i = 0; i < DMA_MAX_DEVICES; i++) if (dma_device_key[i] == device) return dma_device_domain[i] == domain_id;
    return 0;
}
static int dma_validate(u32 device, u32 domain_id, u64 address, u64 length, u32 permissions) {
    DmaDomain *domain = dma_domain(domain_id);
    if (!domain || !dma_device_allows(device, domain_id) ||
        !(dma_backend == DMA_BACKEND_SOFTWARE || dma_backend == DMA_BACKEND_HARDWARE) || dma_hw_faulted || !(permissions & (DMA_READ | DMA_WRITE))) return 0;
    for (u32 i = 0; i < DMA_MAX_MAPS; i++) {
        DmaMapping *mapping = &domain->mappings[i];
        if (mapping->active && (mapping->permissions & permissions) == permissions &&
            dma_contains(mapping->address, mapping->address + mapping->length, address, length)) return 1;
    }
    return 0;
}
static DMA_UNUSED int dma_unmap(u32 domain_id, u32 mapping_id) {
    DmaDomain *domain = dma_domain(domain_id);
    if (!domain || !mapping_id || mapping_id > DMA_MAX_MAPS || !domain->mappings[mapping_id - 1].active) return 0;
#if !defined(__STDC_HOSTED__) || !__STDC_HOSTED__
    if (dma_backend == DMA_BACKEND_HARDWARE) {
        DmaMapping *mapping = &domain->mappings[mapping_id - 1];
        for (u64 page = mapping->address; page < mapping->address + mapping->length; page += DMA_PAGE) {
            u64 *pml4 = (u64 *)(u64)domain->second_level;
            u64 p1 = pml4[(page >> 39) & 511]; if (!p1) continue;
            u64 *pdpt = (u64 *)(u64)(p1 & ~0xfffULL), p2 = pdpt[(page >> 30) & 511]; if (!p2) continue;
            u64 *pd = (u64 *)(u64)(p2 & ~0xfffULL), p3 = pd[(page >> 21) & 511]; if (!p3) continue;
            u64 *pt = (u64 *)(u64)(p3 & ~0xfffULL); pt[(page >> 12) & 511] = 0;
        }
        if (!dma_vtd_flush()) { dma_hw_faulted = 1; return 0; }
    }
#endif
    domain->mappings[mapping_id - 1].active = 0;
    return 1;
}
static DMA_UNUSED int dma_revoke(u32 domain_id) {
    DmaDomain *domain = dma_domain(domain_id); if (!domain) return 0;
#if !defined(__STDC_HOSTED__) || !__STDC_HOSTED__
    if (dma_backend == DMA_BACKEND_HARDWARE) {
        for (u32 i = 0; i < DMA_MAX_MAPS; i++) if (domain->mappings[i].active) {
            DmaMapping *mapping = &domain->mappings[i];
            for (u64 page = mapping->address; page < mapping->address + mapping->length; page += DMA_PAGE) {
                u64 *pml4 = (u64 *)(u64)domain->second_level;
                u64 p1 = pml4[(page >> 39) & 511]; if (!p1) continue;
                u64 *pdpt = (u64 *)(u64)(p1 & ~0xfffULL), p2 = pdpt[(page >> 30) & 511]; if (!p2) continue;
                u64 *pd = (u64 *)(u64)(p2 & ~0xfffULL), p3 = pd[(page >> 21) & 511]; if (!p3) continue;
                u64 *pt = (u64 *)(u64)(p3 & ~0xfffULL); pt[(page >> 12) & 511] = 0;
            }
        }
        if (!dma_vtd_flush()) { dma_hw_faulted = 1; return 0; }
    }
#endif
    for (u32 i = 0; i < DMA_MAX_MAPS; i++) domain->mappings[i].active = 0;
    return 1;
}
static DMA_UNUSED int dma_teardown(u32 domain_id) {
    DmaDomain *domain = dma_domain(domain_id); if (!domain) return 0;
    for (u32 i = 0; i < DMA_MAX_DEVICES; i++) if (dma_device_domain[i] == domain_id) {
        dma_device_key[i] = DMA_DEVICE_NONE; dma_device_domain[i] = DMA_DEVICE_NONE;
    }
    dma_revoke(domain_id); domain->active = 0; return 1;
}

static DMA_UNUSED void dma_fault_poll(void) {
#if !defined(__STDC_HOSTED__) || !__STDC_HOSTED__
    if (dma_backend != DMA_BACKEND_HARDWARE || dma_hw_faulted) return;
    u32 status = (u32)dma_mmio_read(0x34);
    if (status & 0xff) { dma_faults++; dma_fault_status = status; dma_hw_faulted = 1; dma_mmio_write(0x34, status); }
#endif
}

#endif
