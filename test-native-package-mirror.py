"""Host checks for the hash-verified Linux native package mirror fixture."""
import hashlib
from pathlib import Path
import tempfile
from urllib.request import urlopen

from native_package_mirror import NativePackageMirror


with tempfile.TemporaryDirectory(prefix='aurora-native-mirror-') as temporary:
    root = Path(temporary)
    archive = root / 'diffutils-3.10.tar.xz'
    archive.write_bytes(b'verified fixture')
    lock = {'diffutils': {'archive': archive.name,
                          'sha256': hashlib.sha256(archive.read_bytes()).hexdigest()}}
    mirror = NativePackageMirror(root, lock, ['diffutils'])
    mirror.start()
    try:
        with urlopen(f'http://127.0.0.1:{mirror.host_port}/{archive.name}', timeout=2) as response:
            assert response.read() == archive.read_bytes()
    finally:
        mirror.stop()

    archive.write_bytes(b'changed')
    try:
        NativePackageMirror(root, lock, ['diffutils'])
    except FileNotFoundError as error:
        assert 'mirror hash mismatch' in str(error)
    else:
        raise AssertionError('accepted an unverified mirror input')

print('PASS native package mirror: served verified input and rejected hash mismatch')
