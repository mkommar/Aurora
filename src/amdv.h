#ifndef AURORA_AMDV_H
#define AURORA_AMDV_H

/* Minimal AMD-Vi IVRS/IVHD records used by the bounded VirtIO backend. */
#define AMD_IVRS_IVHD 0x10
#define AMD_IVRS_IVHD_EXT 0x11
#define AMD_IVRS_IVHD_40 0x40
#define AMD_IVRS_MAX_DEVICES 32
#define AMDV_UNUSED __attribute__((unused))
#define AMDV_DEV_PERM_READ (1ULL << 61)
#define AMDV_DEV_PERM_WRITE (1ULL << 62)

typedef struct {
    u64 register_base;
    u16 segment;
    u16 devices[AMD_IVRS_MAX_DEVICES];
    u32 device_count;
    u8 include_all;
} AmdvInfo;

static AMDV_UNUSED int amdv_checksum(const u8 *data, u32 length) {
    u8 sum = 0;
    while (length--) sum = (u8)(sum + *data++);
    return sum == 0;
}

static AMDV_UNUSED int amdv_has_device(const AmdvInfo *info, u16 device) {
    if (info->include_all) return 1;
    for (u32 i = 0; i < info->device_count; i++) if (info->devices[i] == device) return 1;
    return 0;
}

static AMDV_UNUSED int amdv_parse_ivrs(const u8 *table, u32 length, AmdvInfo *info) {
    if (!table || !info || length < 48 || __builtin_memcmp(table, "IVRS", 4) ||
        *(const u32 *)(table + 4) != length || !amdv_checksum(table, length)) return 0;
    *info = (AmdvInfo){0};
    for (u32 offset = 48; offset + 4 <= length;) {
        u8 type = table[offset];
        u16 size = *(const u16 *)(table + offset + 2);
        if (size < 24 || offset > length - size) return 0;
        if ((type == AMD_IVRS_IVHD || type == AMD_IVRS_IVHD_EXT || type == AMD_IVRS_IVHD_40) &&
            !info->register_base) {
            const u8 *ivhd = table + offset;
            u64 base = *(const u64 *)(ivhd + 8);
            if (!base || (base & 0xfff)) return 0;
            info->register_base = base;
            info->segment = *(const u16 *)(ivhd + 16);
            for (u32 entry = 24; entry + 4 <= size;) {
                u8 entry_type = ivhd[entry];
                if (entry_type == 0x01) {
                    if (entry + 4 > size || info->device_count == AMD_IVRS_MAX_DEVICES) return 0;
                    info->devices[info->device_count++] = *(const u16 *)(ivhd + entry + 2);
                    entry += 4;
                } else if (entry_type == 0x02) {
                    if (entry + 8 > size) return 0;
                    u16 first = *(const u16 *)(ivhd + entry + 2), last = *(const u16 *)(ivhd + entry + 4);
                    if (last < first) return 0;
                    if ((u32)last - first + 1 > AMD_IVRS_MAX_DEVICES - info->device_count) return 0;
                    for (u32 device = first; device <= last; device++) info->devices[info->device_count++] = (u16)device;
                    entry += 8;
                } else if (entry_type == 0x00) {
                    info->include_all = 1; entry += 4;
                } else {
                    return 0;
                }
            }
        }
        offset += size;
    }
    return info->register_base && (info->include_all || info->device_count);
}

static AMDV_UNUSED u64 amdv_dte(u64 root, u16 domain_id) {
    (void)domain_id;
    return (root & ~0xfffULL) | 3ULL | AMDV_DEV_PERM_READ | AMDV_DEV_PERM_WRITE | (4ULL << 9);
}

static AMDV_UNUSED u64 amdv_table_entry(u64 table, u32 next_level) {
    return (table & ~0xfffULL) | 1ULL | AMDV_DEV_PERM_READ | AMDV_DEV_PERM_WRITE |
           ((u64)(next_level & 7) << 9);
}

static AMDV_UNUSED u64 amdv_pte(u64 physical, u32 permissions) {
    return (physical & ~0xfffULL) | ((permissions & 1) ? AMDV_DEV_PERM_READ : 0) |
           ((permissions & 2) ? AMDV_DEV_PERM_WRITE : 0);
}

static AMDV_UNUSED u32 amdv_fault_type(u64 event) { return (u32)((event >> 48) & 0xffff); }
static AMDV_UNUSED u32 amdv_fault_source(u64 event) { return (u32)(event & 0xffff); }
static AMDV_UNUSED u64 amdv_fault_address(u64 event_address) { return event_address & ~0xfffULL; }

#endif
