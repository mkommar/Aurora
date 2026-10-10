"""Deterministic GPT layout used by Aurora development images."""
import struct
import uuid
import zlib

SECTOR_SIZE = 512
EXT2_START = 2048
EXT2_SECTORS = 1048576
FAT_START = EXT2_START + EXT2_SECTORS
FAT_SECTORS = 262144
# Keep the reserved tail that Aurora's native GPT geometry expects.
IMAGE_SECTORS = FAT_START + FAT_SECTORS + 2048
PARTITIONS = (
    ('0fc63daf-8483-4772-8e79-3d69d8477de4', 'Aurora development', EXT2_START, EXT2_SECTORS),
    ('ebd0a0a2-b9e5-4433-87c0-68b6b72699c7', 'Aurora exchange', FAT_START, FAT_SECTORS),
)


def write_gpt(disk, sectors=IMAGE_SECTORS):
    """Write the protective MBR and both deterministic GPT copies."""
    if sectors != IMAGE_SECTORS:
        raise ValueError('Aurora development GPT has a fixed image size')
    entries = bytearray(128 * 128)
    for index, (kind, name, start, count) in enumerate(PARTITIONS):
        struct.pack_into('<16s16sQQQ72s', entries, index * 128,
                         uuid.UUID(kind).bytes_le,
                         uuid.uuid5(uuid.NAMESPACE_DNS, name).bytes_le,
                         start, start + count - 1, 0,
                         name.encode('utf-16le'))
    entries_crc = zlib.crc32(entries)
    protective = bytearray(SECTOR_SIZE)
    struct.pack_into('<B3sB3sII', protective, 446, 0, b'\0\0\0', 0xee,
                     b'\xff\xff\xff', 1, min(sectors - 1, 0xffffffff))
    protective[510:512] = b'\x55\xaa'

    def header(current, alternate, table):
        data = bytearray(SECTOR_SIZE)
        data[:8] = b'EFI PART'
        struct.pack_into('<II', data, 8, 0x10000, 92)
        struct.pack_into('<QQQQ', data, 24, current, alternate, 34, sectors - 34)
        data[56:72] = uuid.uuid5(uuid.NAMESPACE_DNS, 'Aurora development disk').bytes_le
        struct.pack_into('<QIII', data, 72, table, 128, 128, entries_crc)
        struct.pack_into('<I', data, 16, 0)
        struct.pack_into('<I', data, 16, zlib.crc32(data[:92]))
        return data

    disk.seek(0)
    disk.write(protective)
    for sector, data in ((1, header(1, sectors - 1, 2)),
                         (2, entries),
                         (sectors - 33, entries),
                         (sectors - 1, header(sectors - 1, 1, sectors - 33))):
        disk.seek(sector * SECTOR_SIZE)
        disk.write(data)
