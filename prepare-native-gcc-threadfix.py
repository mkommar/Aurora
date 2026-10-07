"""Create a NEW AURDEV toolchain-image copy with Aurora's pinned musl _Fork fix.

The guest backport workflow normally compiles this fix inside the full GNU
development image. This Linux fallback builds only the replacement archive
member on the host, preserving the original toolchain image and its metadata.
"""
import argparse
import hashlib
import os
from pathlib import Path, PurePosixPath
import platform
import shutil
import struct
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parent
SOURCE_SHA256 = '9b969322012d796dc23dda27a35866034fa67d8fb67e0e2c45c913c3d43219dd'
ORIGINAL_LIBC_SHA256 = '3372ecd3cf1717fd1a78a447c61793b82ebc87a42910c990341c4059b9f29e23'
ROOT_NAME = 'musl-1.2.2'
PREFIXES = ('COPYRIGHT', 'arch/x86_64', 'arch/generic', 'src/internal',
            'src/include', 'include', 'src/process/_Fork.c')


def stage_copy(source, target):
    with Path(source).open('rb') as src, Path(target).open('xb') as dst:
        while block := src.read(1024 * 1024):
            if any(block):
                dst.write(block)
            else:
                dst.seek(len(block), os.SEEK_CUR)
        dst.truncate()


def extract_musl(archive, destination):
    destination = Path(destination)
    with tarfile.open(archive, 'r:gz') as tar:
        for member in tar:
            name = member.name.removeprefix('./')
            if not name.startswith(ROOT_NAME + '/') and name != ROOT_NAME:
                continue
            rel = name[len(ROOT_NAME):].lstrip('/')
            if not rel or not any(rel == prefix or rel.startswith(prefix + '/') for prefix in PREFIXES):
                continue
            path = destination / ROOT_NAME / rel
            if not path.resolve().is_relative_to(destination.resolve()):
                raise ValueError(f'Archive path escapes extraction root: {name}')
            if member.isdir():
                path.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                path.parent.mkdir(parents=True, exist_ok=True)
                with tar.extractfile(member) as incoming, path.open('wb') as outgoing:
                    shutil.copyfileobj(incoming, outgoing)
            elif member.issym():
                link = PurePosixPath(member.linkname)
                target = (path.parent / member.linkname).resolve()
                if link.is_absolute() or not target.is_relative_to((destination / ROOT_NAME).resolve()):
                    raise ValueError(f'Unsafe source symlink: {name}')
                path.parent.mkdir(parents=True, exist_ok=True)
                if not path.exists() and not path.is_symlink():
                    path.symlink_to(member.linkname)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', default='build/toolchain.img')
    parser.add_argument('--output', default='build/toolchain-threadfix.img')
    parser.add_argument('--archive', default='tools/musl-backport/musl-1.2.2.tar.gz')
    parser.add_argument('--musl-include', default=os.environ.get('MUSL_INCLUDE', f'/usr/include/{platform.machine()}-linux-musl'))
    args = parser.parse_args()
    source, output, archive = map(Path, (args.source, args.output, args.archive))
    if output.exists():
        raise SystemExit(f'{output} already exists; preserving existing files')
    raw = archive.read_bytes()
    if hashlib.sha256(raw).hexdigest() != SOURCE_SHA256:
        raise SystemExit('Pinned musl source checksum mismatch')

    # Read the AURDEV table and locate the current libc archive.
    with source.open('rb') as disk:
        magic, count, next_sector = struct.unpack('<8sII', disk.read(16))
        if magic != b'AURDEV01' or count >= 4096:
            raise SystemExit('Expected an AURDEV01 source image with metadata capacity')
        libc_entry = None
        for index in range(count):
            disk.seek((index + 1) * 512)
            meta = bytearray(disk.read(512))
            name, sector, size, capacity, kind = struct.unpack('<256sQQII232x', meta)
            name = name.split(b'\0', 1)[0].decode()
            if name == '/lib/libc.a':
                libc_entry = (index, meta, sector, size, capacity, kind)
    if not libc_entry:
        raise SystemExit('/lib/libc.a is missing from the source toolchain image')

    with tempfile.TemporaryDirectory(prefix='aurora-musl-fork-') as temporary:
        temporary = Path(temporary)
        extract_musl(archive, temporary)
        musl = temporary / ROOT_NAME
        fork_c = musl / 'src/process/_Fork.c'
        before = b'self->tid = __syscall(SYS_gettid);'
        after = b'self->tid = __syscall(SYS_set_tid_address, &__thread_list_lock);'
        original_source = fork_c.read_bytes()
        if original_source.count(before) != 1:
            raise SystemExit('Pinned musl _Fork source did not match expected patch context')
        fork_c.write_bytes(original_source.replace(before, after))
        object_file = temporary / '_Fork.lo'
        subprocess.run(['clang', '-fPIC', '-ffreestanding', '-fno-builtin',
            '-fno-stack-protector', '-O2', '-std=c11', '-D_XOPEN_SOURCE=700',
            f'-I{musl / "arch/x86_64"}', f'-I{musl / "arch/generic"}',
            f'-I{musl / "src/include"}', f'-I{musl / "src/internal"}',
            f'-I{musl / "include"}', f'-I{args.musl_include}',
            '-include', str(ROOT / 'tools-source/musl-fork-abi.h'),
            '-c', str(fork_c), '-o', str(object_file)], cwd=ROOT, check=True)

        original_meta = libc_entry[1]
        old_sector, old_size, _, kind = libc_entry[2:]
        with source.open('rb') as disk:
            disk.seek(old_sector * 512)
            original_libc = disk.read(old_size)
        if hashlib.sha256(original_libc).hexdigest() != ORIGINAL_LIBC_SHA256:
            raise SystemExit('Source toolchain libc archive checksum mismatch')
        old_libc = temporary / 'libc.a'
        old_libc.write_bytes(original_libc)
        subprocess.run(['ar', 'r', str(old_libc), str(object_file)], check=True)
        subprocess.run(['ranlib', str(old_libc)], check=True)
        patched_libc = old_libc.read_bytes()

    # Copy the disk sparsely, then append both the backup and patched archive.
    output.parent.mkdir(parents=True, exist_ok=True)
    stage_copy(source, output)
    slots = [(libc_entry[0], bytearray(original_meta), '/lib/libc.a', patched_libc, kind),
             (count, bytearray(original_meta), '/lib/libc.a.before-thread-fix', original_libc, kind)]
    new_sector = next_sector
    with output.open('r+b') as disk:
        for index, meta, name, payload, file_kind in slots:
            capacity = max(1, (len(payload) + 511) // 512)
            if new_sector + capacity >= 1024 * 1024:
                raise SystemExit('Patched AURDEV image exceeds its 512 MiB bound')
            disk.seek(new_sector * 512)
            disk.write(payload)
            disk.write(bytes(capacity * 512 - len(payload)))
            encoded = name.encode()
            meta[0:256] = encoded + bytes(256 - len(encoded))
            struct.pack_into('<QQII', meta, 256, new_sector, len(payload), capacity, file_kind)
            disk.seek((index + 1) * 512)
            disk.write(meta)
            new_sector += capacity
        disk.seek(0)
        disk.write(struct.pack('<8sII', b'AURDEV01', count + 1, new_sector))
    print(f'Created patched test toolchain image: {output}')


if __name__ == '__main__':
    main()
