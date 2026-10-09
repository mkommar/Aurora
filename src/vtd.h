#ifndef AURORA_VTD_H
#define AURORA_VTD_H

/* Small, testable pieces of Intel VT-d discovery and second-level tables. */
#define VTD_DMAR_DRHD 0
#define VTD_DRHD_INCLUDE_ALL 1
#define VTD_CONTEXT_PRESENT 1ULL
#define VTD_CONTEXT_TRANSLATE 0ULL
#define VTD_CONTEXT_AW_4LEVEL 2ULL
#define VTD_FAULT_RECORD_OFFSET 0x220
#define VTD_READ 1ULL
#define VTD_WRITE 2ULL
#define VTD_UNUSED __attribute__((unused))

typedef struct {
    u64 register_base;
    u16 segment;
    u8 include_all;
} VtdUnit;

static VTD_UNUSED int vtd_checksum(const u8 *data, u32 length) {
    u8 sum = 0;
    while (length--) sum = (u8)(sum + *data++);
    return sum == 0;
}

static VTD_UNUSED int vtd_parse_dmar(const u8 *table, u32 length, VtdUnit *unit) {
    if (!table || !unit || length < 48 || __builtin_memcmp(table, "DMAR", 4) ||
        *(const u32 *)(table + 4) != length || !vtd_checksum(table, length)) return 0;
    for (u32 offset = 48; offset + 4 <= length;) {
        u16 type = *(const u16 *)(table + offset);
        u16 size = *(const u16 *)(table + offset + 2);
        if (size < 16 || offset > length - size) return 0;
        if (type == VTD_DMAR_DRHD && size >= 16) {
            const u8 *drhd = table + offset;
            u64 base = *(const u64 *)(drhd + 8);
            if ((base & 0xfff) == 0 && base) {
                unit->register_base = base;
                unit->segment = *(const u16 *)(drhd + 6);
                unit->include_all = (u8)(drhd[4] & VTD_DRHD_INCLUDE_ALL);
                return 1;
            }
        }
        offset += size;
    }
    return 0;
}

static VTD_UNUSED u64 vtd_context_entry(u64 second_level) {
    return (second_level & ~0xfffULL) | VTD_CONTEXT_PRESENT | VTD_CONTEXT_TRANSLATE;
}

static VTD_UNUSED u64 vtd_context_attributes(u16 domain_id) {
    return VTD_CONTEXT_AW_4LEVEL | ((u64)domain_id << 8);
}

static VTD_UNUSED u64 vtd_leaf_entry(u64 physical, u32 permissions) {
    return (physical & ~0xfffULL) |
           ((permissions & VTD_READ) ? VTD_READ : 0) |
           ((permissions & VTD_WRITE) ? VTD_WRITE : 0);
}

static VTD_UNUSED u32 vtd_fault_reason(u32 fault_status) {
    /* Intel reports the first fault reason in bits 0..7 and the source ID in
       the fault recording register. Keep decoding independent of MMIO. */
    return fault_status & 0xff;
}

static VTD_UNUSED u32 vtd_fault_record_index(u32 fault_status) {
    return (fault_status >> 8) & 0xf;
}

static VTD_UNUSED int vtd_fault_pending(u32 fault_status) {
    return !!(fault_status & (1U << 1));
}

static VTD_UNUSED u32 vtd_fault_record_reason(u64 record_high) {
    return (u32)((record_high >> 32) & 0xff);
}

static VTD_UNUSED u32 vtd_fault_record_source(u64 record_high) {
    return (u32)(record_high & 0xffff);
}

static VTD_UNUSED u64 vtd_fault_record_address(u64 record_low) {
    return record_low & ~0xfffULL;
}

#endif
