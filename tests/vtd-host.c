#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;
typedef unsigned long long u64;
#include "../src/vtd.h"

static void checksum(u8 *table, u32 length) {
    u8 sum = 0;
    for (u32 i = 0; i < length; i++) sum = (u8)(sum + table[i]);
    table[9] = (u8)(table[9] - sum);
}

int main(void) {
    u8 dmar[64] = {0};
    memcpy(dmar, "DMAR", 4);
    *(u32 *)(dmar + 4) = sizeof(dmar);
    dmar[8] = 1;
    *(u16 *)(dmar + 48) = VTD_DMAR_DRHD;
    *(u16 *)(dmar + 50) = 16;
    dmar[52] = VTD_DRHD_INCLUDE_ALL;
    *(u64 *)(dmar + 56) = 0xfed90000ULL;
    checksum(dmar, sizeof(dmar));
    VtdUnit unit;
    assert(vtd_parse_dmar(dmar, sizeof(dmar), &unit));
    assert(unit.register_base == 0xfed90000ULL && unit.include_all);
    assert(vtd_context_entry(0x12345000) == (0x12345000ULL | 1ULL));
    assert(vtd_context_attributes(7) == (2ULL | (7ULL << 8)));
    assert(vtd_leaf_entry(0x2000, VTD_READ | VTD_WRITE) == 0x2003);
    assert(vtd_fault_reason(0x80000031) == 0x31);
    assert(vtd_fault_record_index(0x00000500) == 5);
    assert(vtd_fault_record_reason(0x0000003100000000ULL) == 0x31);
    assert(vtd_fault_record_source(0x0000000012340000ULL) == 0x1234);
    assert(vtd_fault_record_address(0x12345abc) == 0x12345000);
    dmar[50] = 15;
    assert(!vtd_parse_dmar(dmar, sizeof(dmar), &unit));
    puts("PASS VT-d DMAR parsing, context/table construction and fault decoding");
    return 0;
}
