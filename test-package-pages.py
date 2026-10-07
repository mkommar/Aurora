"""Test deterministic Pages publication from synthetic Debian archives."""
import hashlib
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).parent


def make_deb(folder, package='aurora-pages-probe', version='1.0-1'):
    control = folder / 'control'; data = folder / 'data'
    control.mkdir(); data.mkdir(parents=True)
    (control / 'control').write_text(
        f'Package: {package}\nVersion: {version}\nArchitecture: musl-linux-amd64\n'
        'Description: synthetic Pages fixture\n')
    (data / 'opt/aurora/share/pages-probe').mkdir(parents=True)
    (data / 'opt/aurora/share/pages-probe/result').write_text('ok\n')
    for root in (control, data):
        subprocess.run(['tar', '--sort=name', '--mtime=@1720000000', '--owner=0', '--group=0',
            '--numeric-owner', '-czf', str(folder / f'{root.name}.tar.gz'), '-C', str(root), '.'], check=True)
    (folder / 'debian-binary').write_text('2.0\n')
    archive = folder / f'{package}_{version}_musl-linux-amd64.deb'
    subprocess.run(['ar', 'crD', str(archive), 'debian-binary', 'control.tar.gz', 'data.tar.gz'],
        cwd=folder, check=True)
    return archive


def run_generator(package_dir, output, lock):
    return subprocess.run(['python3', 'packages/generate-pages.py', '--packages', str(package_dir),
        '--output', str(output), '--source-lock', str(lock)], cwd=ROOT, text=True,
        capture_output=True)


with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary); package_dir = root / 'packages'; package_dir.mkdir()
    fixture_dir = root / 'fixture'; fixture_dir.mkdir()
    archive = make_deb(fixture_dir)
    shutil.copy2(archive, package_dir / archive.name)
    lock = root / 'sources.lock.json'
    lock.write_text(json.dumps({'schema': 1, 'packages': {'pages-probe': {
        'version': '1.0', 'archive': 'pages-probe-1.0.tar.gz',
        'url': 'https://example.invalid/pages-probe-1.0.tar.gz',
        'sha256': 'a' * 64, 'license': 'MIT', 'recipe': 'fixture'}}}, indent=2) + '\n')
    first = root / 'first'; second = root / 'second'
    assert run_generator(package_dir, first, lock).returncode == 0
    assert run_generator(package_dir, second, lock).returncode == 0
    first_files = sorted(path.relative_to(first) for path in first.rglob('*') if path.is_file())
    second_files = sorted(path.relative_to(second) for path in second.rglob('*') if path.is_file())
    assert first_files == second_files
    assert all((first / path).read_bytes() == (second / path).read_bytes() for path in first_files)
    packages = (first / 'apt/dists/aurora/main/binary-musl-linux-amd64/Packages').read_text()
    assert 'Package: aurora-pages-probe' in packages and 'Filename: pool/' in packages
    assert (first / 'apt/pool' / archive.name).exists()
    source = json.loads((first / 'sources/index.json').read_text())['sources'][0]
    assert source['url'].startswith('https://') and source['license'] == 'MIT'
    manifest = json.loads((first / 'release-manifest.json').read_text())
    assert manifest['packages'][0]['sha256'] == hashlib.sha256(archive.read_bytes()).hexdigest()
    unsafe_dir = root / 'unsafe-input'; unsafe_dir.mkdir()
    unsafe = unsafe_dir / archive.name
    try:
        unsafe.symlink_to(archive)
        assert run_generator(unsafe_dir, root / 'unsafe-output', lock).returncode != 0
    except OSError:
        pass
    mismatch = root / 'mismatch'; mismatch.mkdir()
    bad = make_deb(mismatch, version='9.9-1')
    bad_input = root / 'bad-input'; bad_input.mkdir(); shutil.copy2(bad, bad_input / bad.name)
    assert run_generator(bad_input, root / 'bad-output', lock).returncode != 0
print('PASS deterministic Pages tree, APT metadata, source manifests, safety and mismatch rejection')
