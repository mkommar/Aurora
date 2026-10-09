"""Build Aurora with the native Linux LLVM/NASM toolchain.

This mirrors build.ps1: the target remains x86_64 freestanding code, while
only the host executable names and filesystem/image plumbing differ.
"""
import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
from qemu_iommu import write_dmar

ROOT = Path(__file__).resolve().parent


def executable(name, explicit=None):
    value = explicit or os.environ.get(name.upper().replace('-', '_'))
    if value:
        path = Path(value)
        if path.is_dir():
            path /= name
        return str(path)
    found = shutil.which(name)
    if not found:
        raise SystemExit(f"Missing {name}; install LLVM, NASM and QEMU prerequisites")
    return found


def run(command):
    print('+', ' '.join(str(part) for part in command), flush=True)
    subprocess.run([str(part) for part in command], cwd=ROOT, check=True)


def compile_source(clang, flags, source, output, extra=()):
    run([clang, *flags, *extra, '-c', source, '-o', output])


def pack_files(image, files, fs_lba, image_bytes):
    data = bytearray(image.read_bytes()) if image.exists() else bytearray(image_bytes)
    if len(data) != image_bytes:
        raise SystemExit(f'Expected a {image_bytes // (1024 * 1024)} MiB Aurora disk image')
    base = fs_lba * 512
    data_end = (fs_lba + 8 + 32 * 128) * 512
    if data_end > len(data) or fs_lba != 512 and fs_lba <= 600:
        raise SystemExit('Filesystem layout overlaps the loader or image boundary')
    if data[base:base + 8] != b'AURFS01\0':
        if any(data[base:data_end]):
            raise SystemExit('Unknown filesystem; refusing to overwrite it')
        data[base:base + 8] = b'AURFS01\0'
    for name, source in files.items():
        if not name or len(name) > 31 or any(c not in 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-' for c in name):
            raise SystemExit(f'Invalid filename: {name}')
        payload = Path(source).read_bytes()
        if len(payload) > 65536:
            raise SystemExit(f'File exceeds 64 KiB: {name}')
        slot = empty = -1
        for index in range(32):
            entry = fs_lba * 512 + 512 + index * 64
            size, used = struct.unpack_from('<II', data, entry + 32)
            if used > 1 or size > 65536:
                raise SystemExit('Invalid filesystem metadata')
            stored = bytes(data[entry:entry + 32]).split(b'\0', 1)[0].decode('ascii')
            if not used and empty < 0:
                empty = index
            if used and stored == name:
                slot = index
        if slot < 0:
            slot = empty
        if slot < 0:
            raise SystemExit('Filesystem directory is full')
        entry = fs_lba * 512 + 512 + slot * 64
        data[entry:entry + 64] = b'\0' * 64
        data[entry:entry + len(name)] = name.encode('ascii')
        struct.pack_into('<II', data, entry + 32, len(payload), 1)
        offset = (fs_lba + 8 + slot * 128) * 512
        if offset + 65536 > len(data):
            raise SystemExit('Filesystem file extent exceeds image boundary')
        data[offset:offset + 65536] = b'\0' * 65536
        data[offset:offset + len(payload)] = payload
    image.write_bytes(data)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--llvm-bin', default=os.environ.get('AURORA_LLVM'))
    parser.add_argument('--output', default='build')
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--dma-fault-test', action='store_true')
    args = parser.parse_args()
    out = ROOT / args.output
    out.mkdir(parents=True, exist_ok=True)
    llvm = Path(args.llvm_bin) if args.llvm_bin else None
    clang = executable('clang', str(llvm / 'clang') if llvm else None)
    lld = executable('ld.lld', str(llvm / 'ld.lld') if llvm else None)
    objcopy = executable('llvm-objcopy', str(llvm / 'llvm-objcopy') if llvm else None)
    nm = executable('llvm-nm', str(llvm / 'llvm-nm') if llvm else None)
    nasm = executable('nasm')

    flags = ['--target=x86_64-none-elf', '-std=c11', '-ffreestanding',
             '-fno-stack-protector', '-fno-pic', '-mno-red-zone',
             '-mgeneral-regs-only', '-fno-builtin', '-O2', '-Wall', '-Wextra', '-Werror']
    for source, target, fmt in [('src/boot.asm', 'boot.bin', 'bin'),
                                ('src/loader.asm', 'loader.bin', 'bin'),
                                ('src/entry.asm', 'entry.o', 'elf64'),
                                ('src/traps.asm', 'traps.o', 'elf64'),
                                ('src/user/start.asm', 'user-start.o', 'elf64')]:
        run([nasm, '-f', fmt, ROOT / source, '-o', out / target])
    compile_source(clang, flags, ROOT / 'src/user/lib.c', out / 'user-lib.o')

    services = ['desktop', 'input', 'display', 'storage']
    if args.self_test:
        services.append('probe')
    service_flags = ['-DAURORA_SELF_TEST=1'] if args.self_test else []
    header = ['/* Generated from the separately linked user binaries. */']
    bundle = ['bits 64', 'section .rodata.images',
              'global ap_start_image,ap_start_end', 'ap_start_image:',
              f'incbin "{out / "ap-start.bin"}"', 'ap_start_end:']
    run([nasm, '-f', 'bin', ROOT / 'src/ap_start.asm', '-o', out / 'ap-start.bin'])
    for service in services:
        compile_source(clang, flags + service_flags + ['-Os'], ROOT / f'src/user/{service}.c', out / f'{service}.o')
        run([lld, '-nostdlib', '-T', ROOT / 'src/user/linker.ld', out / 'user-start.o',
             out / f'{service}.o', out / 'user-lib.o', '-o', out / f'{service}.elf'])
        run([objcopy, '-O', 'binary', out / f'{service}.elf', out / f'{service}.bin'])
        symbols = subprocess.check_output([nm, '-n', str(out / f'{service}.elf')], text=True)
        values = {line.split()[2]: line.split()[0] for line in symbols.splitlines() if len(line.split()) == 3}
        for boundary in ('text_end', 'ro_end'):
            header.append(f'#define {service.upper()}_{boundary.upper()} 0x{values["__" + boundary]}ULL')
        header.extend([f'extern const u8 {service}_image[];', f'#define {service}_image_size {Path(out / (service + ".bin")).stat().st_size}ULL'])
        bundle.extend(['align 16', f'global {service}_image', f'{service}_image:', f'incbin "{out / (service + ".bin")}"'])
        if service == 'probe':
            modules = out / 'modules'
            modules.mkdir(exist_ok=True)
            run(['python3', ROOT / 'tools/module-format.py', 'create',
                 '--input', out / 'probe.bin', '--output', modules / 'probe.mod',
                 '--entry', '0', '--text-end', str(int(values['__text_end'], 16) - 0x400000),
                 '--ro-end', str(min(int(values['__ro_end'], 16) - 0x400000,
                                      (out / 'probe.bin').stat().st_size))])
    (out / 'images.h').write_text('\n'.join(header) + '\n')
    (out / 'images.asm').write_text('\n'.join(bundle) + '\n')
    run([nasm, '-f', 'elf64', out / 'images.asm', '-o', out / 'images.o'])

    fs_root = next((ROOT / 'third_party').glob('lwext4-*'))
    fs_flags = ['-Oz', '-DCONFIG_USE_DEFAULT_CFG=1', '-DCONFIG_EXT_FEATURE_SET_LVL=2',
                '-DCONFIG_JOURNALING_ENABLE=0', '-DCONFIG_XATTR_ENABLE=0',
                '-DCONFIG_EXTENTS_ENABLE=0', '-DCONFIG_HAVE_OWN_ERRNO=1',
                '-DCONFIG_DEBUG_PRINTF=0', '-DCONFIG_DEBUG_ASSERT=0',
                '-DCONFIG_BLOCK_DEV_CACHE_SIZE=128', f'-I{fs_root / "include"}', '-Isrc/fs_platform']
    fs_objects = []
    excluded = {'ext4_mkfs', 'ext4_mbr', 'ext4_xattr', 'ext4_extent'}
    for source in sorted((fs_root / 'src').glob('*.c')):
        if source.stem in excluded:
            continue
        target = out / f'{source.stem}.o'
        compile_source(clang, [flag for flag in flags if flag != '-Werror'] + fs_flags + ['-Wno-unused-function', '-Wno-unused-parameter', '-ffunction-sections', '-fdata-sections'], source, target)
        fs_objects.append(target)
    compile_source(clang, flags + fs_flags, ROOT / 'src/fs_platform/runtime.c', out / 'fs-runtime.o')
    fs_objects.append(out / 'fs-runtime.o')
    fat_flags = fs_flags + ['-Ithird_party/fatfs-r016/source', '-DFF_FS_NORTC=0']
    for name in ('ff', 'ffunicode'):
        target = out / f'{name}.o'
        compile_source(clang, [flag for flag in flags if flag != '-Werror'] + fat_flags + ['-ffunction-sections', '-fdata-sections'], ROOT / f'third_party/fatfs-r016/source/{name}.c', target)
        fs_objects.append(target)

    lwip = next((ROOT / 'third_party').glob('lwip-*'))
    net_flags = [f'-I{lwip / "src/include"}', '-Isrc/network_platform', '-Isrc/fs_platform', '-Isrc', '-Oz', '-ffunction-sections', '-fdata-sections']
    net_sources = sorted((lwip / 'src/core').glob('*.c')) + sorted((lwip / 'src/core/ipv4').glob('*.c')) + [lwip / 'src/netif/ethernet.c', ROOT / 'src/network.c']
    net_objects = []
    for source in net_sources:
        target = out / f'net-{source.stem}.o'
        compile_source(clang, [flag for flag in flags if flag != '-Werror'] + net_flags, source, target)
        net_objects.append(target)

    kernel_flags = flags + fat_flags + ['-I', str(out), '-Os']
    if args.dma_fault_test and not args.self_test:
        raise SystemExit('--dma-fault-test requires --self-test')
    if args.dma_fault_test:
        kernel_flags.append('-DAURORA_DMA_FAULT_TEST=1')
    compile_source(clang, kernel_flags + service_flags, ROOT / 'src/kernel.c', out / 'kernel.o')
    run([lld, '-nostdlib', '--gc-sections', '-T', ROOT / 'src/linker.ld', out / 'entry.o', out / 'traps.o', out / 'kernel.o', out / 'images.o', *fs_objects, *net_objects, '-o', out / 'kernel.elf'])
    run([objcopy, '-O', 'binary', out / 'kernel.elf', out / 'kernel.bin'])
    kernel = (out / 'kernel.bin').read_bytes()
    if len(kernel) > 307200:
        raise SystemExit('Kernel + service bundle exceeds loader limit of 600 sectors')
    image = out / 'aurora.img'
    image_bytes = (32 if args.self_test else 16) * 1024 * 1024
    fs_lba = 1024 if args.self_test else 512
    data = bytearray(image.read_bytes()) if image.exists() else bytearray(image_bytes)
    if len(data) != image_bytes:
        raise SystemExit(f'Existing disk image is not {image_bytes // (1024 * 1024)} MiB')
    if args.self_test and 4608 + len(kernel) > fs_lba * 512:
        raise SystemExit('Kernel bundle overlaps the filesystem metadata layout')
    data[:512 * 512] = b'\0' * (512 * 512)
    data[:len((out / 'boot.bin').read_bytes())] = (out / 'boot.bin').read_bytes()
    loader = (out / 'loader.bin').read_bytes(); data[512:512 + len(loader)] = loader
    data[4608:4608 + len(kernel)] = kernel
    image.write_bytes(data)
    for name, source in [('hello', 'apps/hello.c'), ('calc', 'apps/calc.c'), ('filedemo', 'apps/filedemo.c')]:
        app = out / 'apps'; app.mkdir(exist_ok=True)
        app_flags = flags + ['-I', 'sdk/include']
        run([nasm, '-f', 'elf64', ROOT / 'sdk/start.asm', '-o', app / 'start.o'])
        compile_source(clang, app_flags, ROOT / 'sdk/runtime.c', app / 'runtime.o')
        compile_source(clang, app_flags, ROOT / 'src/user/lib.c', app / 'lib.o')
        compile_source(clang, app_flags, ROOT / source, app / f'{name}.o')
        run([lld, '-nostdlib', '-z', 'max-page-size=4096', '-T', ROOT / 'sdk/linker.ld', app / 'start.o', app / f'{name}.o', app / 'runtime.o', app / 'lib.o', '-o', app / f'{name}.elf'])
        if (app / f'{name}.elf').stat().st_size > 65536:
            raise SystemExit('Executable exceeds the initial 64 KiB file limit')
    pack_files(image, {name: out / 'apps' / f'{name}.elf' for name in ('hello', 'calc', 'filedemo')}, fs_lba, image_bytes)
    write_dmar(out / 'qemu-dmar.bin')
    print(f'Built Aurora with Linux tools: {out / "aurora.img"}')


if __name__ == '__main__':
    main()
