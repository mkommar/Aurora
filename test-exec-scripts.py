"""Test kernel interpreter handling through execve, without shell fallback."""
from pathlib import Path
import argparse
from aurora_vm import run

if __name__ == '__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--folder',default='build/exec-script-tests')
    parser.add_argument('--cpus',type=int,default=4);args=parser.parse_args()
    run('tests/exec-scripts.sh', args.folder,cpus=args.cpus,
        files={'/work/exec-scripts.c': Path('tests/exec-scripts.c').read_bytes()})
    assert 'AURORA_EXEC_SCRIPTS_PASS' in (Path(args.folder)/'serial.log').read_text()
