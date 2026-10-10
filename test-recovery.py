"""Fault-injection tests of production GPT/ext2 recovery code, without QEMU.

Builds a freestanding Windows DLL with the same LLVM used by build.ps1.
The block model separates pending writes from durable sectors and cuts power
at every repair write/flush boundary. No real disk image is opened for writes.
"""
import argparse
import ctypes as C
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import zlib

parser = argparse.ArgumentParser()
parser.add_argument('--llvm', default=os.environ.get('AURORA_LLVM') or
    str(Path(shutil.which('clang') or r'C:\Program Files\Unity\Hub\Editor\6000.4.0f1\Editor\Data\PlaybackEngines\AndroidPlayer\NDK\toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe').parent))
args = parser.parse_args()
out = Path('build/recovery-tests'); out.mkdir(parents=True, exist_ok=True)
llvm = Path(args.llvm)
if os.name == 'nt':
    subprocess.run([str(llvm/'clang.exe'), '--target=x86_64-pc-windows-msvc',
        '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-mno-stack-arg-probe',
        '-O2', '-Wall', '-Wextra', '-Werror', '-c', 'tests/recovery-host.c', '-o', str(out/'recovery.obj')], check=True)
    subprocess.run([str(llvm/'ld.lld.exe'), '-flavor', 'link', '/dll', '/noentry', '/nodefaultlib',
        '/out:'+str(out/'recovery.dll'), str(out/'recovery.obj')], check=True)
    library=out/'recovery.dll'
else:
    subprocess.run([str(llvm/'clang'), '-fPIC', '-shared', '-fdeclspec', '-ffreestanding',
        '-fno-builtin', '-fno-stack-protector', '-O2', '-Wall', '-Wextra',
        '-Wno-ignored-attributes', '-Wno-unused-function', '-c', 'tests/recovery-host.c',
        '-o', str(out/'recovery.o')], check=True)
    subprocess.run([str(llvm/'ld.lld'), '-shared', '-o', str(out/'recovery.so'), str(out/'recovery.o')], check=True)
    library=out/'recovery.so'
lib = C.CDLL(str(library.resolve()))
callback_type = C.CFUNCTYPE(C.c_int, C.c_uint32, C.c_void_p, C.c_int)
lib.recovery_setup.argtypes = [C.c_uint64, callback_type]
lib.recovery_count.restype = C.c_uint64
SECTORS = 4096
checks = []
def check(label, condition):
    assert condition, label
    checks.append(label)

def checksum_header(image, lba):
    header = bytearray(image[lba*512:(lba+1)*512])
    struct.pack_into('<I', header, 16, 0)
    struct.pack_into('<I', header, 16, zlib.crc32(header[:92]))
    image[lba*512:(lba+1)*512] = header

def fixture():
    image = bytearray(SECTORS*512)
    image[510:512] = b'\x55\xaa'; image[450] = 0xee
    entries = bytearray(16384)
    entries[:16] = bytes.fromhex('af3dc60f838472478e793d69d8477de4')
    struct.pack_into('<QQ', entries, 32, 64, 1023)
    entries[128:144] = bytes.fromhex('a2a0d0ebe5b9334487c068b6b72699c7')
    struct.pack_into('<QQ', entries, 160, 1024, 2047)
    for lba, other, table in [(1, SECTORS-1, 2), (SECTORS-1, 1, SECTORS-33)]:
        h = bytearray(512); h[:8] = b'EFI PART'
        struct.pack_into('<II', h, 8, 0x10000, 92)
        struct.pack_into('<QQQQ', h, 24, lba, other, 34, SECTORS-34)
        h[56:72] = bytes(range(16))
        struct.pack_into('<QIII', h, 72, table, 128, 128, zlib.crc32(entries))
        image[lba*512:(lba+1)*512] = h
        image[table*512:table*512+len(entries)] = entries
        checksum_header(image, lba)
    return image

class Disk:
    def __init__(self, image, fail=0):
        self.durable = bytearray(image); self.visible = bytearray(image)
        self.events = []; self.snapshots = []; self.fail = fail; self.bad_io = False
        self.callback = callback_type(self.io)
        lib.recovery_setup(len(image)//512, self.callback)
    def io(self, lba, pointer, kind):
        if kind == 0:
            if (lba+1)*512 > len(self.visible): self.bad_io = True; return 0
            C.memmove(pointer, bytes(self.visible[lba*512:(lba+1)*512]), 512); return 1
        self.events.append((kind, lba))
        if len(self.events) == self.fail: return 0
        if kind == 1:
            if (lba+1)*512 > len(self.visible): self.bad_io = True; return 0
            self.visible[lba*512:(lba+1)*512] = C.string_at(pointer, 512)
        elif kind == 2: self.durable[:] = self.visible
        self.snapshots.append(bytes(self.durable))
        # A device may persist individual writes before an explicit flush too.
        if kind == 1: self.snapshots.append(bytes(self.visible))
        return 1

disk = Disk(fixture())
check('valid GPT needs no writes', lib.recovery_gpt() == 1 and not disk.events)
lib.recovery_partition_base.restype = C.c_uint
lib.recovery_partition_sectors.restype = C.c_uint
lib.recovery_fat_base.restype = C.c_uint
lib.recovery_fat_sectors.restype = C.c_uint
check('valid GPT discovers both Aurora partitions',
      lib.recovery_partition_base() == 64 and lib.recovery_partition_sectors() == 960 and
      lib.recovery_fat_base() == 1024 and lib.recovery_fat_sectors() == 1024)
disk = Disk(fixture()[:-512])
check('truncated GPT is rejected without out-of-range I/O', lib.recovery_gpt() == 0 and not disk.events and not disk.bad_io)
for damaged in [1, SECTORS-1]:
    original = fixture(); original[damaged*512] ^= 255
    table = 2 if damaged == 1 else SECTORS-33
    for sector in range(table, table+32): original[sector*512] ^= 255
    disk = Disk(original)
    check(f'repair copy {damaged}', lib.recovery_gpt() == 1 and lib.recovery_count(0) == 1)
    events, snapshots = disk.events[:], disk.snapshots[:]
    check('table flush precedes header publication', events[-3:] == [(2, 0), (1, damaged), (2, 0)])
    survivor = SECTORS-1 if damaged == 1 else 1
    for point in range(1, len(events)+1):
        disk = Disk(original, point)
        check(f'copy {damaged}, injected failure {point}', lib.recovery_gpt() == 0 and lib.recovery_count(1) == 1 and lib.recovery_count(0) == 0)
        check('surviving header untouched', disk.durable[survivor*512:(survivor+1)*512] == original[survivor*512:(survivor+1)*512])
    for point, snapshot in enumerate(snapshots):
        disk = Disk(snapshot)
        check(f'copy {damaged}, restart at boundary {point}', lib.recovery_gpt() == 1 and not disk.bad_io)

# CRC-valid hostile geometry must never reach table I/O or repair writes.
for offset, value in [(32, 100), (40, 2), (48, SECTORS+20), (72, SECTORS+1), (72, 100), (72, 0xffffffffffffffff)]:
    image = fixture(); image[(SECTORS-1)*512] ^= 255
    struct.pack_into('<Q', image, 512+offset, value); checksum_header(image, 1)
    disk = Disk(image)
    check(f'unsafe geometry {offset}={value}', lib.recovery_gpt() == 0 and not disk.events and not disk.bad_io)
for mode in ['overlap', 'out-of-range', 'unknown-type-overlap', 'conflict']:
    image = fixture()
    targets = [(1, 2)] if mode == 'conflict' else [(1, 2), (SECTORS-1, SECTORS-33)]
    for lba, table in targets:
        entries = bytearray(image[table*512:table*512+16384])
        struct.pack_into('<QQ', entries, 160, *((1100, 2047) if mode == 'conflict' else (900, 2047) if 'overlap' in mode else (1024, SECTORS)))
        if mode == 'unknown-type-overlap': entries[128:144] = b'Z'*16
        image[table*512:table*512+16384] = entries
        struct.pack_into('<I', image, lba*512+88, zlib.crc32(entries)); checksum_header(image, lba)
    disk = Disk(image)
    check(mode+' rejected without writes', lib.recovery_gpt() == 0 and not disk.events)

disk = Disk(fixture()); lib.recovery_ext2_reset(1)
check('dirty marker and flush before mutation', lib.recovery_dirty() == 0 and disk.events == [(3, 2), (2, 0)] and not lib.recovery_clean())
disk.events.clear()
check('data flushed before clean marker', lib.recovery_sync() == 0 and disk.events == [(4, 0), (2, 0), (3, 1), (2, 0)] and lib.recovery_clean())
for operation, points, clean in [(lib.recovery_dirty, 2, 1), (lib.recovery_sync, 4, 0)]:
    for point in range(1, points+1):
        disk = Disk(fixture(), point); lib.recovery_ext2_reset(clean)
        check(f'ext2 failure {operation.__name__}:{point} propagated', operation() == 5)
        before = len(disk.events)
        check('failed session blocks mutation and sync', lib.recovery_dirty() == 5 and lib.recovery_sync() == 5 and len(disk.events) == before)

(out/'results.json').write_text(json.dumps({'checks': checks, 'passed': len(checks)}, indent=2)+'\n')
print(f'PASS: {len(checks)} recovery checks')
