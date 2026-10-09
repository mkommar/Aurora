#ifndef AURORA_DMA_H
#define AURORA_DMA_H

/* Capability-scoped DMA ownership. The software backend checks every device
 * range before it is put in a descriptor. It is not a substitute for an
 * IOMMU: the hardware backend deliberately remains unavailable and fails
 * closed until page-table programming and fault handling exist. */
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

typedef struct {
    u64 address, length;
    u32 permissions, active;
} DmaMapping;

typedef struct {
    u32 id, owner, active;
    u64 capability;
    u64 policy_base, policy_end;
    DmaMapping mappings[DMA_MAX_MAPS];
} DmaDomain;

static DmaDomain dma_domains[DMA_MAX_DOMAINS];
static u32 dma_device_key[DMA_MAX_DEVICES];
static u32 dma_device_domain[DMA_MAX_DEVICES];
static int dma_backend;

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

static int dma_init(int backend) {
    for (u32 i = 0; i < DMA_MAX_DOMAINS; i++) dma_domains[i].active = 0;
    for (u32 i = 0; i < DMA_MAX_DEVICES; i++) { dma_device_key[i] = DMA_DEVICE_NONE; dma_device_domain[i] = DMA_DEVICE_NONE; }
    dma_backend = backend == DMA_BACKEND_SOFTWARE ? DMA_BACKEND_SOFTWARE : 0;
    return dma_backend != 0;
}
static int dma_domain_create(u32 owner, u64 capability, u64 policy_base, u64 policy_end) {
    if (!dma_backend || !capability || policy_end <= policy_base ||
        !dma_aligned(policy_base) || !dma_aligned(policy_end)) return 0;
    for (u32 i = 0; i < DMA_MAX_DOMAINS; i++) if (!dma_domains[i].active) {
        dma_domains[i] = (DmaDomain){.id = i + 1, .owner = owner, .capability = capability, .active = 1,
                                     .policy_base = policy_base, .policy_end = policy_end};
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
        dma_device_key[i] = device; dma_device_domain[i] = domain_id; return 1;
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
        dma_backend != DMA_BACKEND_SOFTWARE || !(permissions & (DMA_READ | DMA_WRITE))) return 0;
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
    domain->mappings[mapping_id - 1].active = 0;
    return 1;
}
static DMA_UNUSED int dma_revoke(u32 domain_id) {
    DmaDomain *domain = dma_domain(domain_id); if (!domain) return 0;
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

#endif
