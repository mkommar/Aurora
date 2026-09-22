"""Validate native .deb extraction and reproducible APT indexes in Aurora."""
import argparse
from pathlib import Path
from aurora_vm import run
from image_access import read_ext2_files

parser=argparse.ArgumentParser()
parser.add_argument('--disk',default='build/native-compression-tests/development.img')
parser.add_argument('--folder',default='build/package-repository-tests')
args=parser.parse_args()
folder=Path(args.folder)
disk=run('tests/package-repository.sh',folder,disk=args.disk,cpus=1,port=4455,
    files={'/work/packages/repository.sh':Path('packages/repository.sh').read_bytes()})
paths=['/work/repository-one/'+name for name in ('Packages','Packages.gz','Release','Release.sha256')]
for name,data in read_ext2_files(disk,paths).items():(folder/Path(name).name).write_bytes(data)
