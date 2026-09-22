"""Exercise the bootstrap gate itself using a compiler on a candidate image."""
import argparse
from pathlib import Path
import shlex
from aurora_vm import run

if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--disk',default='build/development.img')
    parser.add_argument('--compiler',default='/bin/gcc')
    parser.add_argument('--folder',default='build/compiler-corpus-tests')
    args=parser.parse_args()
    folder=Path(args.folder);folder.mkdir(parents=True,exist_ok=True)
    script=folder/'job.sh'
    script.write_text('#!/bin/bash\nset -eu\ntrap \'rc=$?; /usr/bin/sync; exit "$rc"\' EXIT\nbash /work/compiler-corpus.sh '+shlex.quote(args.compiler)+'\n',encoding='utf-8',newline='\n')
    files={'/work/'+name:Path('tests',name).read_bytes() for name in ('compiler-corpus.c','compiler-corpus.sh')}
    run(script,folder,disk=args.disk,files=files,cpus=4,port=4458)
