"""Download GNU source inputs and a disposable Alpine builder into tools/.

The builder is separate from Aurora. No host installation is performed.
The lock records SHA-256 for repeat runs; it is integrity pinning, not signature
verification. GNU source archives are retained for redistribution/rebuilding.
"""
from pathlib import Path
import concurrent.futures, hashlib, json, urllib.request

ROOT=Path('tools/gnu-bootstrap'); ROOT.mkdir(parents=True,exist_ok=True)
INPUTS={
 'gzip-1.14.tar.xz':'https://ftp.gnu.org/gnu/gzip/gzip-1.14.tar.xz',
 'linux-virt-6.18.52-r0.apk':'https://dl-cdn.alpinelinux.org/alpine/v3.23/main/x86_64/linux-virt-6.18.52-r0.apk',
 'alpine-minirootfs-3.23.0-x86_64.tar.gz':'https://dl-cdn.alpinelinux.org/alpine/v3.23/releases/x86_64/alpine-minirootfs-3.23.0-x86_64.tar.gz',
 'bash-5.2.37.tar.gz':'https://ftp.gnu.org/gnu/bash/bash-5.2.37.tar.gz',
 'make-4.4.1.tar.gz':'https://ftp.gnu.org/gnu/make/make-4.4.1.tar.gz',
 'coreutils-9.5.tar.xz':'https://ftp.gnu.org/gnu/coreutils/coreutils-9.5.tar.xz',
 'sed-4.9.tar.xz':'https://ftp.gnu.org/gnu/sed/sed-4.9.tar.xz',
 'grep-3.11.tar.xz':'https://ftp.gnu.org/gnu/grep/grep-3.11.tar.xz',
 'gawk-5.3.1.tar.xz':'https://ftp.gnu.org/gnu/gawk/gawk-5.3.1.tar.xz',
 'findutils-4.10.0.tar.xz':'https://ftp.gnu.org/gnu/findutils/findutils-4.10.0.tar.xz',
 'tar-1.35.tar.xz':'https://ftp.gnu.org/gnu/tar/tar-1.35.tar.xz',
}
lockpath=Path('gnu-bootstrap.lock.json')
lock=json.loads(lockpath.read_text()) if lockpath.exists() else {}
def fetch(item):
    name,url=item; path=ROOT/name
    if not path.exists():
        print('Downloading',name,flush=True)
        part=path.with_suffix(path.suffix+'.part')
        with urllib.request.urlopen(url,timeout=90) as response,part.open('wb') as out:
            while chunk:=response.read(1024*1024):out.write(chunk)
        part.replace(path)
    digest=hashlib.sha256(path.read_bytes()).hexdigest()
    if name in lock and lock[name]['sha256']!=digest:raise RuntimeError('Checksum mismatch: '+name)
    return name,{'url':url,'sha256':digest}
if __name__=='__main__':
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        for name,entry in pool.map(fetch,INPUTS.items()):
            lock[name]=entry;lockpath.write_text(json.dumps(lock,indent=2)+'\n');print('Pinned',name,flush=True)
