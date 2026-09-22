"""Exercise actual package-manager lock semantics across Aurora processes."""
import argparse
from pathlib import Path
from aurora_vm import run
parser=argparse.ArgumentParser();parser.add_argument('--folder',default='build/package-lock-tests')
args=parser.parse_args()
run('tests/package-locks.sh',args.folder,cpus=4,port=4454,
    files={'/work/package-locks.c':Path('tests/package-locks.c').read_bytes()})
