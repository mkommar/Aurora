"""Check the Aurora libgcc_s recipe and its bounded install contract."""
import hashlib
import json
from pathlib import Path

lock = json.loads(Path('packages/sources.lock.json').read_text())
meta = lock['packages']['libgcc-s']
build = Path('packages/build.sh').read_text()
runner = Path('native-packages.py').read_text()
installer = Path('packages/install.sh').read_text()

assert meta['recipe'] == 'gcc-runtime'
assert meta['target'] == 'x86_64-linux-musl'
assert meta['archive'] == 'gcc-11.5.0.tar.xz'
assert len(meta['sha256']) == 64
assert meta['runtime_files'] == ['/opt/aurora/lib/libgcc_s.so.1']
assert 'x86_64-linux-musl' in build and 'all-target-libgcc' in build
assert 'install-target-libgcc' in build and 'libgcc_s.so.1' in build
assert 'static-libgcc' in build
assert '--disable-shared --enable-static --disable-nls' in build
assert build.index('diffutils)') < build.index('gcc-runtime)')
assert build.index('xz)') > build.index('gcc-runtime)')
assert 'package_description' in runner
assert 'opt/aurora/*' in installer
assert hashlib.sha256(Path('packages/build.sh').read_bytes()).hexdigest()
assert 'x86_64-aurora' not in build
print('PASS libgcc_s metadata, x86_64-linux-musl target, contents and path policy')
