"""Exercise actual package-manager lock semantics across Aurora processes."""
import argparse
from pathlib import Path
from aurora_vm import run
parser=argparse.ArgumentParser();parser.add_argument('--folder',default='build/package-lock-tests');parser.add_argument('--package-binary',help='stage a static musl package-lock test instead of compiling it inside Aurora')
args=parser.parse_args()
files={'/work/package-locks.c':Path('tests/package-locks.c').read_bytes()}
script='tests/package-locks.sh';launch='bash aurora-job.sh'
if args.package_binary:
    files['/work/package-locks']=Path(args.package_binary).read_bytes();script='tests/package-locks-prebuilt.sh';launch='./package-locks'
run(script,args.folder,cpus=4,port=4454,files=files,launch=launch)
