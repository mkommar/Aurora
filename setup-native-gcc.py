"""Create Aurora's optional native GCC disk. Never replaces an existing disk.
Uses a pinned, SHA-512 verified static musl.cc GCC 11.2.1 toolchain.
Only regular-file contents are copied; archive paths are never extracted to disk.
"""
import argparse, hashlib, json, posixpath, struct, tarfile, urllib.request
from pathlib import Path

URL='https://musl.cc/x86_64-linux-musl-native.tgz'
SHA512='44d441ad9aa11a06feddf3daa4c9f53ad7d9ca37af1f5a61379aca07793703d179410cea723c1b7fca94c4de19a321228bdb3656bc5cbdb5e3bea8e2d6dac6c7'
p=argparse.ArgumentParser();p.add_argument('--image',default='build/toolchain.img');args=p.parse_args()
image=Path(args.image)
if image.exists():raise SystemExit(f'{image} already exists; preserving its contents.')
archive=Path('tools/gcc-native/x86_64-linux-musl-native.tgz');archive.parent.mkdir(parents=True,exist_ok=True)
if not archive.exists():
    print('Downloading pinned GCC toolchain...',flush=True);urllib.request.urlretrieve(URL,archive)
if hashlib.sha512(archive.read_bytes()).hexdigest()!=SHA512:raise SystemExit('GCC archive checksum mismatch')
t=tarfile.open(archive);members={m.name.rstrip('/'):m for m in t.getmembers()}
root='x86_64-linux-musl-native'
def content(m,seen=None):
    seen=set() if seen is None else seen
    if m.name in seen:raise ValueError('Archive link cycle')
    seen.add(m.name)
    if m.isfile():return t.extractfile(m).read()
    if m.islnk():return content(members[m.linkname],seen)
    if m.issym():
        target=posixpath.normpath(posixpath.join(posixpath.dirname(m.name),m.linkname))
        if target in members and not members[target].isdir():return content(members[target],seen)
    return None
files={};directories={'/','/bin','/tmp','/work','/dev','/proc','/proc/self'}
for name,m in members.items():
    path='/'+name[len(root):].lstrip('/')
    # C-only runtime/toolchain; omit manuals, C++, Fortran, shared libraries.
    if path.startswith('/share/') or '/include/c++/' in path or path.endswith(('.so','.so.1')):continue
    if path.startswith('/bin/') and not path.endswith(('/gcc','/as','/ld','/x86_64-linux-musl-gcc')):continue
    if '/libexec/' in path and not path.endswith(('/cc1','/collect2')):continue
    if any(s in path for s in ('libstdc++','libgfortran','libquadmath','/f951','/cc1plus','/plugin/')):continue
    blob=content(m)
    if blob is None:continue
    files[path]=blob
    parent=posixpath.dirname(path)
    while parent!='/':directories.add(parent);parent=posixpath.dirname(parent)
files['/work/demo.c']=b'#include <stdio.h>\nint main(void) { puts("Compiled by GCC inside Aurora!"); return 0; }\n'
files['/work/compute.c']=b'#include <stdio.h>\n#include <stdlib.h>\nint main(void) { int *p=malloc(4000); if(!p) return 1; int sum=0; for(int i=0;i<1000;i++){p[i]=i;sum+=p[i];} printf("sum=%d\\n",sum); free(p); return sum!=499500; }\n'
files['/work/broken.c']=b'int main(void) { this is not valid C; }\n'
files['/work/math.c']=b'#include <stdio.h>\nint main(void) { volatile double a=1.5,b=2.0; printf("fp=%.1f\\n",a*b); return a*b!=3.0; }\n'
files['/dev/null']=b''
entries=[];sector=4097;image.parent.mkdir(parents=True,exist_ok=True)
with image.open('xb') as out:
    out.truncate(512*1024*1024)
    for path in sorted(directories):entries.append((path,0,0,0,2))
    for path,blob in sorted(files.items()):
        capacity=max(1,(len(blob)+511)//512)
        out.seek(sector*512);out.write(blob)
        entries.append((path,sector,len(blob),capacity,1));sector+=capacity
    if len(entries)>4096:raise ValueError('Too many files')
    out.seek(0);out.write(struct.pack('<8sII',b'AURDEV01',len(entries),sector))
    for i,(path,start,size,capacity,kind) in enumerate(entries):
        encoded=path.encode()
        if len(encoded)>255:raise ValueError(path)
        out.seek((i+1)*512);out.write(struct.pack('<256sQQII232x',encoded,start,size,capacity,kind))
manifest={'source':URL,'sha512':SHA512,'gcc':'11.2.1 (20211120)','files':len(entries),'data_bytes':sector*512}
image.with_suffix('.manifest.json').write_text(json.dumps(manifest,indent=2))
print(f'Created {image}: {len(entries)} entries, {sector*512//1048576} MiB populated')
