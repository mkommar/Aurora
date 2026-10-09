#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;
typedef unsigned long long u64;
#include "../src/amdv.h"

static void checksum(u8 *table, u32 length) { u8 sum = 0; for (u32 i = 0; i < length; i++) sum = (u8)(sum + table[i]); table[9] = (u8)(table[9] - sum); }

int main(void) {
    u8 ivrs[84] = {0};
    memcpy(ivrs, "IVRS", 4); *(u32 *)(ivrs + 4) = sizeof(ivrs); ivrs[8] = 1;
    *(u16 *)(ivrs + 48) = AMD_IVRS_IVHD; *(u16 *)(ivrs + 50) = 36;
    *(u64 *)(ivrs + 56) = 0xfed90000ULL;
    *(u16 *)(ivrs + 72) = 1; *(u16 *)(ivrs + 74) = 24;
    *(u16 *)(ivrs + 76) = 1; *(u16 *)(ivrs + 78) = 32;
    *(u16 *)(ivrs + 80) = 1; *(u16 *)(ivrs + 82) = 40;
    checksum(ivrs, sizeof(ivrs));
    AmdvInfo info;
    assert(amdv_parse_ivrs(ivrs, sizeof(ivrs), &info));
    assert(info.register_base == 0xfed90000ULL && info.device_count == 3);
    assert(amdv_has_device(&info, 24) && amdv_has_device(&info, 40) && !amdv_has_device(&info, 48));
    assert(amdv_dte(0x120000, 3) == (0x120000ULL | 3 | (4ULL << 9)));
    assert(amdv_pte(0x4000, 3) == 0x4003 && amdv_fault_type(2ULL << 28) == 2);
    assert(amdv_fault_source(0x1234) == 0x1234 && amdv_fault_address(0x12345abc) == 0x12345000);
    ivrs[50] = 15; assert(!amdv_parse_ivrs(ivrs, sizeof(ivrs), &info));
    puts("PASS AMD-Vi IVRS parsing, device IDs, DTE/PTE construction and fault decoding");
    return 0;
}
