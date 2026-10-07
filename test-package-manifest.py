"""Check pinned inputs and patch bytes before sending a native build to QEMU."""
import hashlib
import importlib.util
import json
from pathlib import Path
from urllib.parse import urlparse

lock=json.loads(Path('packages/sources.lock.json').read_bytes())
packages=lock['packages']
bootstrap={'bootstrap-gcc','bootstrap-g++','bootstrap-bash','bootstrap-make',
    'bootstrap-coreutils','bootstrap-tar','bootstrap-gzip'}
visited=set();active=set()
def visit(name):
    if name in bootstrap or name in visited:return
    if name in active:raise ValueError('Circular source dependency: '+name)
    active.add(name)
    for dependency in packages[name]['build_dependencies']:visit(dependency)
    active.remove(name);visited.add(name)

count=0
for name,meta in packages.items():
    visit(name)
    assert urlparse(meta['url']).scheme=='https'
    assert Path(meta['archive']).name==meta['archive'] and '/' not in meta['archive']
    assert len(meta['sha256'])==64 and all(c in '0123456789abcdef' for c in meta['sha256'])
    assert meta['license'] and meta['recipe']
    for dependency in meta.get('runtime_dependencies',[]):assert dependency in packages
    candidates=[Path(root)/meta['archive'] for root in ('tools/native-sources','tools/gnu-bootstrap','tools/musl-backport')]
    existing=next((p for p in candidates if p.exists()),None)
    if existing:
        assert hashlib.sha256(existing.read_bytes()).hexdigest()==meta['sha256'],name
        count+=1

spec=importlib.util.spec_from_file_location('native_packages','native-packages.py')
runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(runner)
files,script=runner.payload(list(packages))
assert not any(name.endswith(('.tar.gz','.tar.xz','.tgz')) for name in files)
assert files['/work/packages-install.sh']==Path('packages/install.sh').read_bytes()
assert 'cp /work/packages-install.sh /work/packages/install.sh' in script
assert '/work/packages/musl-0.patch' in files
patch=files['/work/packages/musl-0.patch']
assert b'SYS_set_tid_address' in patch and b'\r' not in patch
assert hashlib.sha256(patch).hexdigest()==packages['musl']['patches'][0]['sha256']
for invalid in ('../make','make;sync','missing'):
    try:runner.payload([invalid])
    except ValueError:pass
    else:raise AssertionError('Accepted invalid package '+invalid)
make_env=dict(line.split('=',1) for line in files['/work/package-make.env'].decode().splitlines())
assert make_env['recipe_sha256']==hashlib.sha256(Path('packages/build.sh').read_bytes()+b'\0'+Path('packages/install.sh').read_bytes()).hexdigest()
print(f'PASS {len(packages)} package manifests, dependency graph, {count} cached source hashes, patch pins and recipe-only staging')
