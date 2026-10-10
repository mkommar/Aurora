"""Host regression for Aurora's deterministic development GPT layout."""
from io import BytesIO
import struct
import zlib

from development_image import (EXT2_START, EXT2_SECTORS, FAT_START,
                                FAT_SECTORS, IMAGE_SECTORS, write_gpt)


def inspect():
    image = BytesIO(b'\0' * (IMAGE_SECTORS * 512))
    write_gpt(image)
    first = image.getvalue()
    image.seek(0)
    write_gpt(image)
    assert first == image.getvalue(), 'GPT output must be deterministic'
    assert first[510:512] == b'\x55\xaa'
    assert first[512:520] == b'EFI PART' and first[-512:-504] == b'EFI PART'
    for header_offset, alternate in ((512, IMAGE_SECTORS - 1),
                                     ((IMAGE_SECTORS - 1) * 512, 1)):
        header = bytearray(first[header_offset:header_offset + 512])
        size, checksum = struct.unpack_from('<II', header, 12)
        struct.pack_into('<I', header, 16, 0)
        assert size == 92 and zlib.crc32(header[:size]) == checksum
        assert struct.unpack_from('<Q', header, 32)[0] == alternate
        table, count, stride, entries_crc = struct.unpack_from('<QIII', header, 72)
        entries = first[table * 512:table * 512 + count * stride]
        assert (count, stride) == (128, 128) and zlib.crc32(entries) == entries_crc
    assert struct.unpack_from('<QQ', first, 1024 + 32) == (EXT2_START, EXT2_START + EXT2_SECTORS - 1)
    assert struct.unpack_from('<QQ', first, 1024 + 128 + 32) == (FAT_START, FAT_START + FAT_SECTORS - 1)


inspect()
print('PASS deterministic development GPT layout and reciprocal copies')
