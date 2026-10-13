"""Stage reviewed recipes, then download/configure/build/package INSIDE Aurora.

No source archives or configure results are imported by this runner.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import threading
import time
from aurora_vm import run
from image_access import read_ext2_files
from native_package_mirror import NativePackageMirror
from transaction_observability import tail, write_checkpoint


def observe(folder, stop):
    """Persist bounded host status while aurora_vm owns the QEMU process."""
    folder = Path(folder)
    state = folder / 'state'
    state.mkdir(parents=True, exist_ok=True)
    write_checkpoint(state, 'guest-boot', 'running', 'waiting for serial markers')
    while not stop.is_set():
        serial = folder / 'serial.log'
        text = serial.read_text(errors='replace') if serial.exists() else ''
        (folder / 'serial.tail').write_text(text[-4000:], encoding='utf-8')
        qemu = []
        for proc in Path('/proc').glob('[0-9]*'):
            try:
                command = (proc / 'cmdline').read_bytes().replace(b'\0', b' ').decode()
                if 'qemu-system' in command:
                    status = (proc / 'status').read_text(errors='replace')
                    rss = next((line.split(':', 1)[1].strip() for line in status.splitlines()
                                if line.startswith('VmRSS:')), 'unknown')
                    qemu.append(f'pid={proc.name} rss={rss} cmd={command[:240]}')
            except (OSError, UnicodeError):
                pass
        (folder / 'host-status').write_text(
            f'updated={int(time.time())}\nserial_bytes={len(text)}\n'
            f'cpus={os.cpu_count() or 1}\n' + '\n'.join(qemu) + '\n', encoding='utf-8')
        if 'desktop ready' in text:
            write_checkpoint(state, 'guest-boot', 'passed', 'desktop ready')
        if 'NET: DHCP' in text:
            write_checkpoint(state, 'network-readiness', 'passed', 'DHCP marker observed')
        stop.wait(5)


def harvest(folder,names):
    """Retain guest evidence after the runner has stopped its QEMU process."""
    lock=json.loads(Path('packages/sources.lock.json').read_bytes())['packages']
    destination=folder/'artifacts';destination.mkdir(exist_ok=True)
    for name in names:
        version=lock[name]['version']
        paths=[f'/work/packages/logs/{name}.log',
            f'/work/packages/build/{name}-{version}/obj/config.log',
            f'/work/packages/out/aurora-{name}_{version}-1_musl-linux-amd64.deb',
            f'/work/packages/out/{name}.sha256']
        for path in paths:
            try:
                data=read_ext2_files(folder/'development.img',[path])[path]
                target=destination/(name+'-config.log' if path.endswith('/config.log') else Path(path).name)
                target.write_bytes(data)
            except (OSError,RuntimeError,ValueError,AssertionError) as error:
                # A failing configure or interrupted extraction may not have
                # produced later artifacts. Preserve the original build result.
                print(f'Artifact unavailable: {path}: {error}',flush=True)


def harvest_status(folder, disk, names):
    """Copy bounded guest status out of the disposable disk after shutdown."""
    destination = Path(folder) / 'state'
    destination.mkdir(parents=True, exist_ok=True)
    for name in names:
        for suffix in ('current.json', 'last-observed.json', 'checkpoints.jsonl', 'heartbeat', 'proc-status', 'serial-tail.log'):
            guest = f'/work/packages/state/{name}/{suffix}'
            try:
                data = read_ext2_files(disk, [guest])[guest]
            except (OSError, RuntimeError, ValueError, AssertionError):
                continue
            target = destination / name
            target.mkdir(parents=True, exist_ok=True)
            (target / suffix).write_bytes(data[-1024 * 1024:])


def payload(names,retry=False,mirror_url='',stage_timeout=1800):
    raw=Path('packages/sources.lock.json').read_bytes()
    lock=json.loads(raw)['packages']
    recipe=Path('packages/build.sh').read_bytes()
    install_recipe=Path('packages/install.sh').read_bytes()
    recipe_digest=hashlib.sha256(recipe+b'\0'+install_recipe).hexdigest()
    files={'/work/packages-lock.json':raw,'/work/packages-build.sh':recipe}
    files['/work/packages-install.sh']=install_recipe
    files['/work/packages/build-observed.sh']=Path('packages/build-observed.sh').read_bytes()
    files['/work/packages/repository.sh']=Path('packages/repository.sh').read_bytes()
    files['/etc/resolv.conf']=b'nameserver 10.0.2.3\noptions timeout:2 attempts:2\n'
    for filename in ('compiler-corpus.c','compiler-corpus.sh'):
        files['/work/'+filename]=Path('tests',filename).read_bytes()
    script=['#!/bin/bash','set -eu','mkdir -p /work/packages',
        'cp /work/packages-lock.json /work/packages/sources.lock.json',
        'cp /work/packages-build.sh /work/packages/build.sh',
        'cp /work/packages-install.sh /work/packages/install.sh']
    for name in names:
        if not re.fullmatch(r'[a-z0-9+-]+',name) or name not in lock: raise ValueError('Unknown package '+name)
        meta=lock[name]
        if not re.fullmatch(r'[a-f0-9]{64}',meta['sha256']): raise ValueError('Missing SHA-256')
        env={k:meta[k] for k in ('version','archive','url','sha256','license','recipe')}
        env['recipe_sha256']=recipe_digest
        patches=[]
        for number,patch in enumerate(meta['patches']):
            root=Path('packages').resolve();source=(root/patch['file']).resolve()
            if not source.is_relative_to(root): raise ValueError('Patch escapes package directory')
            data=source.read_bytes()
            if hashlib.sha256(data).hexdigest()!=patch['sha256']: raise ValueError('Patch checksum mismatch')
            if type(patch['strip']) is not int or not 0<=patch['strip']<=8: raise ValueError('Invalid patch strip count')
            target=f'/work/packages/{name}-{number}.patch'
            files[target]=data
            patches.append(f"{patch['sha256']} {patch['strip']} {target}")
        env['patch_specs']='\n'.join(patches)
        # Build dependencies are recorded separately; only explicitly declared
        # runtime dependencies become Debian Depends fields.
        env['depends']=', '.join('aurora-'+n for n in meta.get('runtime_dependencies',[]))
        env['package_description']=meta.get('description',name)
        env['mirror_url']=mirror_url
        envtext=''.join(k+'='+shlex.quote(v)+'\n' for k,v in env.items())
        files['/work/package-'+name+'.env']=envtext.encode()
        script+=['cp /work/package-'+name+'.env /work/packages/'+name+'.env']
        if retry:
            stem=shlex.quote(name+'-'+meta['version'])
            script+=['stem='+stem,'attempt=1',
                'while test -e "/work/packages/build/$stem.failed-$attempt"; do attempt=$((attempt+1)); done',
                'for tree in build stage; do if test -e "/work/packages/$tree/$stem"; then mv "/work/packages/$tree/$stem" "/work/packages/$tree/$stem.failed-$attempt"; fi; done',
                'if test -f /work/packages/logs/'+name+'.log; then mv /work/packages/logs/'+name+'.log "/work/packages/logs/'+name+'.failed-$attempt.log"; fi']
        script+=['AURORA_STAGE_TIMEOUT='+str(stage_timeout)+' bash /work/packages/build-observed.sh '+shlex.quote(name)]
    script+=['sync','echo AURORA_NATIVE_PACKAGES_COMPLETE']
    return files,'\n'.join(script)+'\n'


if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('packages',nargs='+')
    parser.add_argument('--folder',default='build/native-package-tests')
    parser.add_argument('--disk',default='build/native-build.img')
    parser.add_argument('--resume',action='store_true')
    parser.add_argument('--retry',action='store_true',help='preserve failed trees/logs and start a clean build on a resumed candidate')
    parser.add_argument('--timeout',type=int,default=14400)
    parser.add_argument('--stage-timeout',type=int,default=1800,
                        help='maximum seconds without a guest checkpoint update')
    parser.add_argument('--cpus',type=int,default=1)
    parser.add_argument('--port',type=int,default=4456)
    parser.add_argument('--mirror-cache',help='serve hash-verified source archives through QEMU NAT')
    parser.add_argument('--mirror-port',type=int,default=8080)
    args=parser.parse_args()
    if args.retry and not args.resume: parser.error('--retry requires --resume')
    mirror=None
    if args.mirror_cache:
        source_lock=json.loads(Path('packages/sources.lock.json').read_bytes())['packages']
        mirror=NativePackageMirror(args.mirror_cache,source_lock,args.packages,args.mirror_port)
        mirror.start()
    mirror_url=mirror.guest_url(mirror.host_port) if mirror else ''
    files,script=payload(args.packages,args.retry,mirror_url,args.stage_timeout)
    path=Path(args.folder);path.mkdir(parents=True,exist_ok=True)
    job=path/'job.sh';job.write_text(script,encoding='utf-8',newline='\n')
    stop = threading.Event()
    observer = threading.Thread(target=observe, args=(path, stop), daemon=True)
    observer.start()
    try:
        run(job,path,disk=args.disk,resume=args.resume,files=files,timeout=args.timeout,cpus=args.cpus,port=args.port,network_ready=True)
    finally:
        stop.set()
        observer.join(timeout=6)
    harvest(path,args.packages)
    candidates = [item for item in Path(path).glob('*.img') if item.name != 'aurora.img']
    if candidates:
        harvest_status(path,candidates[0],args.packages)
    if mirror: mirror.stop()
