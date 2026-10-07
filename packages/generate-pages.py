"""Build a deterministic GitHub Pages tree for Aurora packages."""
import argparse
import gzip
import hashlib
import io
import json
import re
import shutil
import subprocess
import tarfile
from pathlib import Path

ARCHITECTURE = 'musl-linux-amd64'
PACKAGE_RE = re.compile(r'^aurora-([a-z0-9+-]+)_([^_]+)-1_musl-linux-amd64\.deb$')
SAFE_NAME = re.compile(r'^[a-zA-Z0-9.+_~-]+$')


def control_from_deb(path):
    raw = subprocess.check_output(['ar', 'p', str(path), 'control.tar.gz'])
    with tarfile.open(fileobj=io.BytesIO(gzip.decompress(raw)), mode='r:') as archive:
        member = archive.extractfile('./control') or archive.extractfile('control')
        if member is None:
            raise ValueError(f'{path}: control file is missing')
        fields = {}
        for line in member.read().decode('utf-8').splitlines():
            if not line or line[0].isspace() or ':' not in line:
                raise ValueError(f'{path}: invalid control field')
            key, value = line.split(':', 1)
            if key in fields:
                raise ValueError(f'{path}: duplicate control field {key}')
            fields[key] = value.strip()
    return fields


def source_record(name, metadata):
    return {
        'name': name,
        'version': metadata['version'],
        'archive': metadata['archive'],
        'url': metadata['url'],
        'sha256': metadata['sha256'],
        'license': metadata['license'],
        'build_dependencies': metadata.get('build_dependencies', []),
        'runtime_dependencies': metadata.get('runtime_dependencies', []),
        'recipe': metadata['recipe'],
    }


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n', encoding='utf-8')


def build(lock_path, package_dir, output):
    lock_bytes = lock_path.read_bytes()
    lock = json.loads(lock_bytes)
    packages = lock.get('packages')
    if not isinstance(packages, dict) or not packages:
        raise ValueError('source lock must contain a non-empty packages object')
    if output.exists():
        raise ValueError(f'output already exists: {output}')
    if not package_dir.is_dir():
        raise ValueError(f'package input directory does not exist: {package_dir}')

    candidates = sorted(package_dir.iterdir(), key=lambda path: path.name)
    archives = [path for path in candidates if path.suffix == '.deb']
    if not archives:
        raise ValueError(f'no .deb files found in {package_dir}')
    records = []
    names = set()
    for source in archives:
        if source.is_symlink() or not source.is_file() or not SAFE_NAME.fullmatch(source.name):
            raise ValueError(f'unsafe package input: {source.name}')
        match = PACKAGE_RE.fullmatch(source.name)
        if not match:
            raise ValueError(f'invalid Aurora package filename: {source.name}')
        name, version = match.groups()
        if name in names:
            raise ValueError(f'duplicate package: {name}')
        names.add(name)
        if name not in packages:
            raise ValueError(f'{source.name}: package is absent from sources.lock.json')
        expected_version = packages[name]['version']
        if version != expected_version:
            raise ValueError(f'{source.name}: filename version does not match lock ({expected_version})')
        fields = control_from_deb(source)
        expected_package = f'aurora-{name}'
        if fields.get('Package') != expected_package:
            raise ValueError(f'{source.name}: control Package is not {expected_package}')
        if fields.get('Version') != f'{version}-1' or fields.get('Architecture') != ARCHITECTURE:
            raise ValueError(f'{source.name}: control metadata does not match filename')
        data = source.read_bytes()
        records.append((source, fields, data, hashlib.sha256(data).hexdigest()))

    apt = output / 'apt'
    pool = apt / 'pool'
    binary = apt / 'dists' / 'aurora' / 'main' / f'binary-{ARCHITECTURE}'
    pool.mkdir(parents=True)
    binary.mkdir(parents=True)
    package_lines = []
    package_manifest = []
    for source, fields, data, digest in records:
        shutil.copyfile(source, pool / source.name)
        for key, value in fields.items():
            package_lines.append(f'{key}: {value}')
        package_lines.extend([
            f'Filename: pool/{source.name}', f'Size: {len(data)}', f'SHA256: {digest}', ''
        ])
        package_manifest.append({'filename': source.name, 'package': fields['Package'],
            'version': fields['Version'], 'architecture': fields['Architecture'],
            'sha256': digest, 'size': len(data), 'path': f'apt/pool/{source.name}'})
    packages_bytes = ('\n'.join(package_lines) + '\n').encode()
    (binary / 'Packages').write_bytes(packages_bytes)
    with (binary / 'Packages.gz').open('wb') as stream:
        with gzip.GzipFile(fileobj=stream, mode='wb', mtime=0, compresslevel=9) as compressed:
            compressed.write(packages_bytes)
    release_entries = []
    for name in ('Packages', 'Packages.gz'):
        item = binary / name
        release_entries.append(f' {hashlib.sha256(item.read_bytes()).hexdigest()} {item.stat().st_size} '
            f'dists/aurora/main/binary-{ARCHITECTURE}/{name}')
    release = ('Origin: Aurora\nLabel: Aurora native packages\nSuite: aurora\n'
        f'Codename: aurora\nArchitectures: {ARCHITECTURE}\nComponents: main\nSHA256:\n'
        + '\n'.join(release_entries) + '\n').encode()
    (apt / 'dists' / 'aurora' / 'Release').write_bytes(release)
    (apt / 'dists' / 'aurora' / 'Release.sha256').write_text(
        hashlib.sha256(release).hexdigest() + '  Release\n', encoding='ascii')

    source_records = [source_record(name, packages[name]) for name in sorted(packages)]
    source_index = {'schema': 1, 'mirroring': 'not performed by this generator', 'sources': source_records}
    write_json(output / 'sources' / 'index.json', source_index)
    for record in source_records:
        write_json(output / 'sources' / f"{record['name']}.json", record)
    manifest = {'schema': 1, 'generator': 'packages/generate-pages.py',
        'source_lock_sha256': hashlib.sha256(lock_bytes).hexdigest(),
        'packages': package_manifest, 'source_index': 'sources/index.json',
        'apt': {'release': 'apt/dists/aurora/Release', 'pool': 'apt/pool',
            'packages': f'apt/dists/aurora/main/binary-{ARCHITECTURE}/Packages'}}
    write_json(output / 'release-manifest.json', manifest)
    (output / 'index.html').write_text("""<!doctype html>
<meta charset="utf-8"><title>Aurora packages</title>
<h1>Aurora packages</h1>
<p>Candidate native packages for Aurora OS, built for musl-linux-amd64.</p>
<h2>Install repository metadata</h2>
<pre>curl -fsSL https://&lt;owner&gt;.github.io/&lt;repository&gt;/apt/dists/aurora/Release</pre>
<p>After adding this URL to an authenticated APT configuration, install with
<code>apt install aurora-make</code>, or download a package directly from
<code>apt/pool/</code>.</p>
<p>The repository is unsigned. Authenticate <code>Release.sha256</code> or use a signed
release policy before installing packages. APT metadata is under <a href="apt/">apt/</a>.</p>
<h2>Downloads and sources</h2>
<p>See <a href="release-manifest.json">release-manifest.json</a> for package hashes and
<a href="sources/index.json">sources/index.json</a> for pinned source URLs, versions,
hashes and licenses. Source archives are not mirrored by this generator.</p>
""", encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--packages', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--source-lock', type=Path, default=Path('packages/sources.lock.json'))
    args = parser.parse_args()
    try:
        build(args.source_lock, args.packages, args.output)
    except (OSError, subprocess.CalledProcessError, ValueError, json.JSONDecodeError) as error:
        parser.error(str(error))
