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


if __name__ == '__main__':
    write_dmar(__import__('pathlib').Path(sys.argv[1] if len(sys.argv) > 1 else 'build/qemu-dmar.bin'))
