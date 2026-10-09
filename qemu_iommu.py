"""QEMU Intel IOMMU fixture shared by the guest regression harnesses."""
import struct
import sys


def write_dmar(path):
    table = bytearray(64)
    table[:4] = b'DMAR'
    struct.pack_into('<I', table, 4, len(table))
    table[8] = 1
    table[10:16] = b'AURORA'
    table[16:24] = b'VTDTEST0'
    struct.pack_into('<I', table, 24, 1)
    table[28:32] = b'AURR'
    struct.pack_into('<I', table, 32, 1)
    struct.pack_into('<HH', table, 48, 0, 16)
    table[52] = 1
    struct.pack_into('<Q', table, 56, 0xFED90000)
    table[9] = (-sum(table)) & 0xff
    path.write_bytes(table)


def write_ivrs(path):
    table = bytearray(84)
    table[:4] = b'IVRS'
    struct.pack_into('<I', table, 4, len(table))
    table[8] = 1
    table[10:16] = b'AURORA'
    table[16:24] = b'AMDTEST0'
    struct.pack_into('<I', table, 24, 1)
    table[28:32] = b'AURR'
    struct.pack_into('<I', table, 32, 1)
    struct.pack_into('<I', table, 36, 0)
    struct.pack_into('<HH', table, 48, 0x10, 36)
    struct.pack_into('<Q', table, 56, 0xfed80000)
    struct.pack_into('<HH', table, 72, 0x01, 24)
    struct.pack_into('<HH', table, 76, 0x01, 32)
    struct.pack_into('<HH', table, 80, 0x01, 40)
    table[9] = (-sum(table)) & 0xff
    path.write_bytes(table)


if __name__ == '__main__':
    from pathlib import Path
    output = Path(sys.argv[1] if len(sys.argv) > 1 else 'build/qemu-dmar.bin')
    if len(sys.argv) > 2 and sys.argv[2] == 'ivrs': write_ivrs(output)
    else: write_dmar(output)
