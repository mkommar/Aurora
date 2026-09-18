"""Bounded artifact access to Aurora's existing 512 MiB ext2 GPT partition.
Reads reject writes; callers stage writes onto copies. Never formats a disk.
"""
import ctypes as C,struct,zlib
from pathlib import Path


def read_ext2_files(image,paths):
    """Read canonical ext2 paths; reject every write at the callback boundary."""
    lib=C.CDLL(str(Path('build/image-tool/ext2-image.dll').resolve()))
    callback_type=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_uint64,C.c_uint32,C.c_int)
    lib.au_attach.argtypes=[callback_type]
    lib.au_get.argtypes=[C.c_char_p,C.c_void_p,C.c_uint64,C.POINTER(C.c_uint64)]
    def check(result):
        if result:raise RuntimeError(f'ext2 image read error {result}')
    with Path(image).open('rb') as disk:
        disk.seek(512);header=bytearray(disk.read(512));assert header[:8]==b'EFI PART'
        length,checksum=struct.unpack_from('<II',header,12);assert 92<=length<=512
        struct.pack_into('<I',header,16,0);assert zlib.crc32(header[:length])==checksum
        lba,count,size,crc=struct.unpack_from('<QIII',header,72);assert count<=128 and size==128
        disk.seek(lba*512);entries=disk.read(count*size);assert zlib.crc32(entries)==crc
        start,end=struct.unpack_from('<QQ',entries,32);assert end-start+1==1048576 and (end+1)*512<=disk.seek(0,2)
        @callback_type
        def transfer(pointer,sector,count,write):
            if write:return 30
            try:
                if sector+count>1048576:return 5
                disk.seek((start+sector)*512);data=disk.read(count*512)
                if len(data)!=count*512:return 5
                C.memmove(pointer,data,len(data));return 0
            except Exception:return 5
        lib.au_attach(transfer);check(lib.au_mount_readonly())
        result={}
        try:
            for path in paths:
                size=C.c_uint64();check(lib.au_get(path.encode(),None,0,C.byref(size)))
                if size.value>64*1024*1024:raise ValueError('Artifact exceeds 64 MiB extraction limit')
                data=C.create_string_buffer(size.value);check(lib.au_get(path.encode(),data,len(data),C.byref(size)))
                result[path]=data.raw[:size.value]
        finally:check(lib.au_close())
        return result

def put_ext2_files(image,files):
    lib=C.CDLL(str(Path('build/image-tool/ext2-image.dll').resolve()))
    callback_type=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_uint64,C.c_uint32,C.c_int)
    lib.au_attach.argtypes=[callback_type];lib.au_put.argtypes=[C.c_char_p,C.c_void_p,C.c_uint64,C.c_uint32]
    def check(result):
        if result:raise RuntimeError(f'ext2 image error {result}')
    with Path(image).open('r+b') as disk:
        disk.seek(512);header=bytearray(disk.read(512));assert header[:8]==b'EFI PART'
        length,checksum=struct.unpack_from('<II',header,12);assert 92<=length<=512
        struct.pack_into('<I',header,16,0);assert zlib.crc32(header[:length])==checksum
        lba,count,size,crc=struct.unpack_from('<QIII',header,72);assert count<=128 and size==128
        disk.seek(lba*512);entries=disk.read(count*size);assert zlib.crc32(entries)==crc
        start,end=struct.unpack_from('<QQ',entries,32);assert end-start+1==1048576 and (end+1)*512<=disk.seek(0,2)
        disk.seek(start*512+1024+56);assert disk.read(2)==b'\x53\xef'
        @callback_type
        def transfer(pointer,sector,count,write):
            try:
                if sector+count>1048576:return 5
                disk.seek((start+sector)*512)
                if write:disk.write(C.string_at(pointer,count*512))
                else:
                    data=disk.read(count*512)
                    if len(data)!=count*512:return 5
                    C.memmove(pointer,data,len(data))
                return 0
            except Exception:return 5
        lib.au_attach(transfer);check(lib.au_mount())
        try:
            for path,data in files.items():check(lib.au_put(path.encode(),data,len(data),0o755 if path.endswith('.sh') else 0o644))
        finally:check(lib.au_close())
        disk.flush()
