"""Grow a NEW copy of the development image for native source builds.

Linux is used only for offline e2fsck/resize2fs, never package compilation.
The source image is opened read-only. Existing destinations are refused.
"""
import argparse
import functools
import http.server
import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess
import threading
import zlib


def grow(source, target, mib):
    source, target = Path(source).resolve(), Path(target).resolve()
    if target.exists() or source == target:
        raise ValueError('Destination must be a new file')
    if not 1024 <= mib <= 32768:
        raise ValueError('Choose 1024..32768 MiB')
    with source.open('rb') as disk:
        total = disk.seek(0, 2)//512
        disk.seek(512); header = bytearray(disk.read(512))
        if header[:8] != b'EFI PART': raise ValueError('Missing GPT')
        size, crc = struct.unpack_from('<II', header, 12)
        struct.pack_into('<I', header, 16, 0)
        if size != 92 or zlib.crc32(header[:size]) != crc: raise ValueError('Bad GPT header')
        table, count, stride, crc = struct.unpack_from('<QIII', header, 72)
        if (table, count, stride) != (2, 128, 128): raise ValueError('Unexpected GPT layout')
        disk.seek(table*512); entries = bytearray(disk.read(count*stride))
        if zlib.crc32(entries) != crc: raise ValueError('Bad GPT entries')
        if any(entries[256:]): raise ValueError('Only two partitions supported')
        start, end = struct.unpack_from('<QQ', entries, 32)
        fat_start, fat_end = struct.unpack_from('<QQ', entries, 160)
        new_count = mib*2048
        if not (start == 2048 and start <= end < fat_start <= fat_end < total-33 and new_count > end-start+1):
            raise ValueError('Unexpected partition bounds or non-growing request')
        new_fat = start+new_count
        fat_count = fat_end-fat_start+1
        new_total = new_fat+fat_count+2048
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open('x+b') as out:
            disk.seek(0); shutil.copyfileobj(disk, out, 1024*1024)
            out.truncate(new_total*512)
            disk.seek(fat_start*512); out.seek(new_fat*512)
            remaining = fat_count*512
            while remaining:
                data = disk.read(min(remaining, 1024*1024))
                if not data: raise EOFError('Truncated FAT partition')
                out.write(data); remaining -= len(data)
            struct.pack_into('<Q', entries, 40, new_fat-1)
            struct.pack_into('<QQ', entries, 160, new_fat, new_fat+fat_count-1)
            struct.pack_into('<Q', header, 48, new_total-34)
            struct.pack_into('<I', header, 88, zlib.crc32(entries))
            for current, backup, location in ((1,new_total-1,2),(new_total-1,1,new_total-33)):
                struct.pack_into('<QQ', header, 24, current, backup)
                struct.pack_into('<Q', header, 72, location)
                struct.pack_into('<I', header, 16, 0)
                struct.pack_into('<I', header, 16, zlib.crc32(header[:92]))
                out.seek(location*512); out.write(entries)
                out.seek(current*512); out.write(header)
            out.seek(458); out.write(struct.pack('<I',new_total-1))
    resize_ext2(target)
    return target


def resize_ext2(target):
    spec=importlib.util.spec_from_file_location('builder','build-gnu-bootstrap.py')
    builder=importlib.util.module_from_spec(spec); spec.loader.exec_module(builder)
    builder.initrd()
    root=target.parent/'resize-tools'; root.mkdir(exist_ok=True)
    (root/'bootstrap-gnu.sh').write_text('''#!/bin/sh
set -eu
apk add --no-cache e2fsprogs e2fsprogs-extra
modprobe virtio_blk
rc=0; e2fsck -f -p /dev/vda1 || rc=$?
test "$rc" -le 1
resize2fs /dev/vda1
e2fsck -f -n /dev/vda1
sync
echo AURORA_BUILD_VOLUME_READY
''',encoding='utf-8',newline='\n')
    server=http.server.ThreadingHTTPServer(('127.0.0.1',8879),functools.partial(http.server.SimpleHTTPRequestHandler,directory=str(root)))
    threading.Thread(target=server.serve_forever,daemon=True).start()
    log=root/'serial.log'
    command=['tools/qemu/qemu-system-x86_64.exe','-machine','pc','-accel','whpx','-cpu','qemu64','-m','512M',
        '-kernel',str(builder.ROOT/'vmlinuz-virt'),'-initrd',str(builder.ROOT/'builder-initramfs.gz'),
        '-append','console=ttyS0 rdinit=/init panic=1','-netdev','user,id=net0','-device','virtio-net-pci,netdev=net0',
        '-display','none','-serial',f'file:{log}','-drive',f'format=raw,file={target},if=none,id=resize',
        '-device','virtio-blk-pci,drive=resize','-no-reboot']
    try:
        process=subprocess.Popen(command,creationflags=subprocess.CREATE_NO_WINDOW)
        try: code=process.wait(timeout=600)
        except BaseException: process.terminate(); process.wait(); raise
        output=log.read_text(errors='replace'); print(output[-4000:])
        if code or 'AURORA_BUILD_VOLUME_READY' not in output or 'Fix? no' in output:
            raise RuntimeError('Offline resize failed; destination must not be used')
    finally:
        server.shutdown(); server.server_close()


if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--source',default='build/development.img')
    parser.add_argument('--target',default='build/native-build.img')
    parser.add_argument('--mib',type=int,default=8192)
    args=parser.parse_args()
    print(grow(args.source,args.target,args.mib))
