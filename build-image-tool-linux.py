"""Build the pinned lwext4/FatFs host image helper as a Linux shared object."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent
EXT4 = ROOT / 'third_party/lwext4-58bcf89a121b72d4fb66334f1693d3b30e4cb9c5'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--llvm-bin', default=os.environ.get('AURORA_LLVM'))
    parser.add_argument('--output', default='build/image-tool')
    args = parser.parse_args()
    out = ROOT / args.output
    out.mkdir(parents=True, exist_ok=True)
    compiler = Path(args.llvm_bin) / 'clang' if args.llvm_bin else Path(shutil.which('clang') or '')
    if not compiler.is_file():
        raise SystemExit('clang was not found; install LLVM or pass --llvm-bin')
    flags = ['-fPIC', '-fdeclspec', '-fno-builtin', '-fno-stack-protector', '-O2',
             '-DCONFIG_USE_DEFAULT_CFG=1', '-DCONFIG_EXT_FEATURE_SET_LVL=2',
             '-DCONFIG_JOURNALING_ENABLE=0', '-DCONFIG_XATTR_ENABLE=1',
             '-DCONFIG_EXTENTS_ENABLE=0', '-DCONFIG_HAVE_OWN_ERRNO=1',
             '-DCONFIG_DEBUG_PRINTF=0', '-DCONFIG_DEBUG_ASSERT=0',
             f'-I{EXT4 / "include"}', '-Isrc/fs_platform',
             '-Ithird_party/fatfs-r016/source', '-U_WIN32', '-U_WIN64',
             '-ffunction-sections', '-fdata-sections', '-Wno-ignored-attributes']
    sources = [source for source in sorted((EXT4 / 'src').glob('*.c'))
               if source.stem not in ('ext4_extent', 'ext4_mbr')]
    sources += [ROOT / 'tools-source/ext2-image.c', ROOT / 'tools-source/fat-image.c',
                ROOT / 'third_party/fatfs-r016/source/ff.c',
                ROOT / 'third_party/fatfs-r016/source/ffunicode.c']
    objects = []
    for source in sources:
        obj = out / f'{source.stem}.o'
        subprocess.run([str(compiler), *flags, '-c', str(source), '-o', str(obj)], cwd=ROOT, check=True)
        objects.append(str(obj))
    library = out / 'ext2-image.so'
    subprocess.run([str(compiler), '-shared', '-Wl,--gc-sections', '-o', str(library), *objects], cwd=ROOT, check=True)
    print(f'Built Linux image helper: {library}')


if __name__ == '__main__': main()
