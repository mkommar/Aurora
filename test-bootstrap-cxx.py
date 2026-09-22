"""Restore the pinned bootstrap C++ components and test them inside Aurora.

This is explicitly an imported bootstrap compiler, not a native GCC rebuild.
The normal development image and its C compiler are not modified.
"""
import gzip
import hashlib
import io
from pathlib import Path, PurePosixPath
import posixpath
import tarfile
from aurora_vm import run

SHA512='44d441ad9aa11a06feddf3daa4c9f53ad7d9ca37af1f5a61379aca07793703d179410cea723c1b7fca94c4de19a321228bdb3656bc5cbdb5e3bea8e2d6dac6c7'


def archive():
    data=Path('tools/gcc-native/x86_64-linux-musl-native.tgz').read_bytes()
    if hashlib.sha512(data).hexdigest()!=SHA512: raise ValueError('Bootstrap compiler SHA-512 mismatch')
    output=io.BytesIO()
    # Decompress once: resolving hundreds of archive links otherwise repeatedly
    # seeks backwards through gzip's stream.
    with tarfile.open(fileobj=io.BytesIO(gzip.decompress(data))) as source, tarfile.open(fileobj=output,mode='w') as target:
        members={m.name.rstrip('/'):m for m in source}
        def content(name,seen):
            if name in seen: raise ValueError('Archive link cycle')
            seen=seen|{name};m=members[name]
            if m.isfile(): return source.extractfile(m).read()
            if m.islnk(): return content(m.linkname,seen)
            if m.issym(): return content(posixpath.normpath(posixpath.join(posixpath.dirname(name),m.linkname)),seen)
            raise ValueError('Unsupported archive member '+name)
        for name,m in sorted(members.items()):
            path=name.removeprefix('x86_64-linux-musl-native/')
            if not (path in ('bin/g++','bin/c++','bin/x86_64-linux-musl-g++','bin/x86_64-linux-musl-c++') or
                    path.startswith('include/c++/') or path.endswith('/cc1plus') or
                    path in ('lib/libstdc++.a','lib/libstdc++fs.a','lib/libsupc++.a')): continue
            if m.isdir(): continue
            if PurePosixPath(path).is_absolute() or '..' in PurePosixPath(path).parts: raise ValueError('Unsafe archive path')
            blob=content(name,set());entry=tarfile.TarInfo(path)
            entry.mode=0o755 if path.startswith('bin/') or path.endswith('/cc1plus') else 0o644
            entry.size=len(blob);target.addfile(entry,io.BytesIO(blob))
    return gzip.compress(output.getvalue(),mtime=0)


if __name__ == '__main__':
    folder=Path('build/bootstrap-cxx-tests');folder.mkdir(exist_ok=True)
    payload=archive()
    script=folder/'job.sh'
    script.write_text('''#!/bin/bash
set -eu
trap 'rc=$?; /usr/bin/sync; exit "$rc"' EXIT
tar xzf /src/bootstrap-cxx.tar.gz -C /
cd /work
g++ -O2 -static bootstrap-cxx.cc -o bootstrap-cxx
./bootstrap-cxx
echo AURORA_BOOTSTRAP_CXX_PASS
''',encoding='utf-8',newline='\n')
    code=b'''#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>
int main(){std::vector<int> v={3,1,2};std::sort(v.begin(),v.end());
if(v[0]!=1||v[2]!=3)return 1;
try{throw std::runtime_error("exception");}catch(const std::exception&){std::cout<<"C++ bootstrap works\\n";return 0;}
return 2;}
'''
    run(script,folder,cpus=1,port=4457,files={'/src/bootstrap-cxx.tar.gz':payload,'/work/bootstrap-cxx.cc':code})
