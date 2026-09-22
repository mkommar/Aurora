"""Stage reviewed recipes, then download/configure/build/package INSIDE Aurora.

No source archives or configure results are imported by this runner.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
from aurora_vm import run


def payload(names):
    raw=Path('packages/sources.lock.json').read_bytes()
    lock=json.loads(raw)['packages']
    recipe=Path('packages/build.sh').read_bytes()
    files={'/work/packages-lock.json':raw,'/work/packages-build.sh':recipe}
    files['/etc/resolv.conf']=b'nameserver 10.0.2.3\noptions timeout:2 attempts:2\n'
    script=['#!/bin/bash','set -eu','mkdir -p /work/packages',
        'cp /work/packages-lock.json /work/packages/sources.lock.json',
        'cp /work/packages-build.sh /work/packages/build.sh']
    for name in names:
        if not re.fullmatch(r'[a-z0-9+-]+',name) or name not in lock: raise ValueError('Unknown package '+name)
        meta=lock[name]
        if meta['patches']: raise ValueError('Patch application must be implemented before building patched packages')
        if not re.fullmatch(r'[a-f0-9]{64}',meta['sha256']): raise ValueError('Missing SHA-256')
        env={k:meta[k] for k in ('version','archive','url','sha256','license','recipe')}
        env['recipe_sha256']=hashlib.sha256(recipe).hexdigest()
        # Build dependencies are recorded separately; only explicitly declared
        # runtime dependencies become Debian Depends fields.
        env['depends']=', '.join('aurora-'+n for n in meta.get('runtime_dependencies',[]))
        envtext=''.join(k+'='+shlex.quote(v)+'\n' for k,v in env.items())
        files['/work/package-'+name+'.env']=envtext.encode()
        script+=['cp /work/package-'+name+'.env /work/packages/'+name+'.env',
            'bash /work/packages/build.sh '+shlex.quote(name)]
    script+=['sync','echo AURORA_NATIVE_PACKAGES_COMPLETE']
    return files,'\n'.join(script)+'\n'


if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('packages',nargs='+')
    parser.add_argument('--folder',default='build/native-package-tests')
    parser.add_argument('--disk',default='build/native-build.img')
    parser.add_argument('--resume',action='store_true')
    parser.add_argument('--timeout',type=int,default=14400)
    parser.add_argument('--cpus',type=int,default=1)
    args=parser.parse_args()
    files,script=payload(args.packages)
    path=Path(args.folder);path.mkdir(parents=True,exist_ok=True)
    job=path/'job.sh';job.write_text(script,encoding='utf-8',newline='\n')
    run(job,path,disk=args.disk,resume=args.resume,files=files,timeout=args.timeout,cpus=args.cpus,port=4456)
