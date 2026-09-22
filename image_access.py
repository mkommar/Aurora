"""Bounded artifact access to Aurora's existing ext2 GPT partition.
Reads reject writes; callers stage writes onto copies. Never formats a disk.
"""
import ctypes as C,struct,zlib,os
from pathlib import Path, PurePosixPath

def geometry(disk, lib):
    disk.seek(512);header=bytearray(disk.read(512));assert header[:8]==b'EFI PART'
    length,checksum=struct.unpack_from('<II',header,12);assert 92<=length<=512
    struct.pack_into('<I',header,16,0);assert zlib.crc32(header[:length])==checksum
    lba,count,size,crc=struct.unpack_from('<QIII',header,72);assert 0<count<=128 and size==128
    disk.seek(lba*512);entries=disk.read(count*size);assert zlib.crc32(entries)==crc
    start,end=struct.unpack_from('<QQ',entries,32)
    assert start>=34 and end>=start and (end+1)*512<=disk.seek(0,2)
    sectors=end-start+1
    lib.au_geometry.argtypes=[C.c_uint64];lib.au_geometry(sectors)
    return start,sectors


def read_ext2_files(image,paths):
    """Read canonical ext2 paths; reject every write at the callback boundary."""
    lib=C.CDLL(str(Path(os.environ.get('AURORA_IMAGE_TOOL','build/image-tool/ext2-image.dll')).resolve()))
    callback_type=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_uint64,C.c_uint32,C.c_int)
    lib.au_attach.argtypes=[callback_type]
    lib.au_get.argtypes=[C.c_char_p,C.c_void_p,C.c_uint64,C.POINTER(C.c_uint64)]
    def check(result):
        if result:raise RuntimeError(f'ext2 image read error {result}')
    with Path(image).open('rb') as disk:
        start,sectors=geometry(disk,lib)
        @callback_type
        def transfer(pointer,sector,count,write):
            if write:return 30
            try:
                if sector+count>sectors:return 5
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
    lib=C.CDLL(str(Path(os.environ.get('AURORA_IMAGE_TOOL','build/image-tool/ext2-image.dll')).resolve()))
    callback_type=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_uint64,C.c_uint32,C.c_int)
    lib.au_attach.argtypes=[callback_type];lib.au_put.argtypes=[C.c_char_p,C.c_void_p,C.c_uint64,C.c_uint32]
    lib.au_mkdir.argtypes=[C.c_char_p]
    def check(result):
        if result:raise RuntimeError(f'ext2 image error {result}')
    with Path(image).open('r+b') as disk:
        start,sectors=geometry(disk,lib)
        disk.seek(start*512+1024+56);assert disk.read(2)==b'\x53\xef'
        @callback_type
        def transfer(pointer,sector,count,write):
            try:
                if sector+count>sectors:return 5
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
            directories={'/'}
            for path,data in files.items():
                canonical=PurePosixPath(path)
                if not canonical.is_absolute() or '..' in canonical.parts:raise ValueError('Expected an absolute canonical path')
                for parent in reversed(canonical.parents):
                    name=str(parent)
                    if name not in directories:
                        error=lib.au_mkdir(name.encode())
                        if error not in (0,17):raise RuntimeError(f'ext2 mkdir {name}: error {error}')
                        directories.add(name)
                executable=path.endswith('.sh') or path in ('/bin/curl','/work/rebuild-network.sh')
                error=lib.au_put(path.encode(),data,len(data),0o755 if executable else 0o644)
                if error:raise RuntimeError(f'ext2 image write {path}: error {error}')
        finally:check(lib.au_close())
        disk.flush()
