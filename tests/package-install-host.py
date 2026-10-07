"""Host fixture for the candidate package installer's validation/rollback path."""
import hashlib
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
INSTALLER = ROOT / 'packages/install.sh'


def archive(folder, files, control_files=None):
    folder = Path(folder)
    folder.mkdir(parents=True)
    payload = folder / 'payload'
    payload.mkdir()
    checksums = []
    for name, data in files.items():
        target = payload / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        checksums.append(f'{hashlib.sha256(data).hexdigest()}  {name}\n')
    package_files = sorted(control_files if control_files is not None else files)
    control = folder / 'control'
    control.mkdir()
    (control / 'control').write_text('Package: aurora-make\nVersion: 4.4.1-1\nArchitecture: musl-linux-amd64\n')
    (control / 'files').write_text(''.join(name + '\n' for name in package_files))
    (control / 'sha256sums').write_text(''.join(checksums))
    for source, output in ((control, folder / 'control.tar.gz'), (payload, folder / 'data.tar.gz')):
        with tarfile.open(output, 'w:gz') as tar:
            tar.add(source, arcname='.')
    (folder / 'debian-binary').write_text('2.0\n')
    deb = folder / 'package.deb'
    subprocess.run(['ar', 'crD', str(deb), 'debian-binary', 'control.tar.gz', 'data.tar.gz'], cwd=folder, check=True)
    digest = folder / 'package.sha256'
    digest.write_text(f'{hashlib.sha256(deb.read_bytes()).hexdigest()}  {deb.name}\n')
    return deb, digest


def install(deb, digest, root, succeeds, env=None):
    result = subprocess.run(['bash', str(INSTALLER), str(deb), str(digest), str(root)],
                            capture_output=True, text=True, env=env)
    assert (result.returncode == 0) == succeeds, result.stdout + result.stderr
    return result


with tempfile.TemporaryDirectory(prefix='aurora-package-install-test-') as temp:
    temp = Path(temp)
    payload = {'opt/aurora/bin/make': b'installed make\n',
               'opt/aurora/share/doc/make/copyright': b'license\n'}
    deb, digest = archive(temp / 'valid', payload)
    root = temp / 'root'; root.mkdir()
    result = install(deb, digest, root, True)
    assert 'AURORA_PACKAGE_INSTALLED aurora-make 4.4.1-1' in result.stdout
    assert (root / 'opt/aurora/bin/make').read_bytes() == payload['opt/aurora/bin/make']
    assert (root / 'var/lib/aurora/packages/aurora-make_4.4.1-1.files').exists()

    result = install(deb, digest, root, False)
    assert 'Refusing to overwrite' in result.stderr

    bad_root = temp / 'bad-checksum-root'; bad_root.mkdir()
    bad_digest = temp / 'bad.sha256'; bad_digest.write_text('0' * 64 + '  package.deb\n')
    result = install(deb, bad_digest, bad_root, False)
    assert 'Package archive checksum mismatch' in result.stderr
    assert not (bad_root / 'opt').exists()

    conflict_payload = {'opt/aurora/bin/make': b'first\n',
                        'opt/aurora/share/doc/make/copyright': b'second\n'}
    conflict_deb, conflict_digest = archive(temp / 'conflict', conflict_payload)
    conflict_root = temp / 'conflict-root'; conflict_root.mkdir()
    target = conflict_root / 'opt/aurora/share/doc/make/copyright'
    target.parent.mkdir(parents=True); target.write_text('preserve\n')
    install(conflict_deb, conflict_digest, conflict_root, False)
    assert not (conflict_root / 'opt/aurora/bin/make').exists()
    assert target.read_text() == 'preserve\n'

    rollback_payload = {'opt/aurora/bin/make': b'first\n',
                        'opt/aurora/share/doc/make/copyright': b'second\n'}
    rollback_deb, rollback_digest = archive(temp / 'rollback', rollback_payload)
    rollback_root = temp / 'rollback-root'; rollback_root.mkdir()
    wrapper_bin = temp / 'wrapper-bin'; wrapper_bin.mkdir()
    move_state = temp / 'move-count'
    wrapper = wrapper_bin / 'mv'
    wrapper.write_text('''#!/bin/bash
count=0
test ! -f "$MOVE_STATE" || read -r count < "$MOVE_STATE"
count=$((count+1));printf '%s\\n' "$count" > "$MOVE_STATE"
test "$count" -ne 2 || exit 1
exec /bin/mv "$@"
''')
    wrapper.chmod(0o755)
    env = dict(os.environ, PATH=f'{wrapper_bin}:/usr/bin:/bin', MOVE_STATE=str(move_state))
    install(rollback_deb, rollback_digest, rollback_root, False, env)
    assert not (rollback_root / 'opt/aurora/bin/make').exists()
    assert not (rollback_root / 'var/lib/aurora/packages/aurora-make_4.4.1-1.files').exists()

    traversal, traversal_digest = archive(temp / 'traversal',
        {'opt/aurora/../../escape': b'bad\n'})
    traversal_root = temp / 'traversal-root'; traversal_root.mkdir()
    result = install(traversal, traversal_digest, traversal_root, False)
    assert ('Unsafe package path' in result.stderr or 'escapes /opt/aurora' in result.stderr), result.stdout + result.stderr
    assert not (temp / 'escape').exists()

print('PASS package install: verified install, duplicate refusal, checksum rejection, collision preflight, rollback and traversal rejection')
