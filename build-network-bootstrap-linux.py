"""Build the pinned static curl/Mbed TLS bootstrap with Linux host tools.

All source archives are checked against network-sources.lock.json before use.
This creates bootstrap binaries for a disposable guest image; it is not a claim
that these libraries were compiled inside Aurora. The curl executable may use
Aurora's existing musl dynamic loader; its TLS backend is statically linked.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parent


def copy_tree(source, entries, prefix):
    source = Path(source)
    for path in source.rglob('*'):
        relative = path.relative_to(source)
        if path.is_dir():
            continue
        if path.is_symlink():
            raise ValueError(f'Unexpected build symlink: {path}')
        entries[f'{prefix}/{relative.as_posix()}'] = path.read_bytes()


def write_bundle(root, entries):
    archive = root / 'network-bootstrap.tar.gz'
    with tarfile.open(archive, 'w:gz') as output:
        for name, data in sorted(entries.items()):
            path = PurePosixPath(name)
            if path.is_absolute() or '..' in path.parts:
                raise ValueError(f'Unsafe bundle path: {name}')
            info = tarfile.TarInfo(name)
            info.size = len(data); info.mtime = 0; info.uid = info.gid = 0
            info.uname = info.gname = ''; info.mode = 0o755 if path.name == 'curl' else 0o644
            import io
            output.addfile(info, io.BytesIO(data))
    return archive


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', default='tools/network-bootstrap')
    parser.add_argument('--sources', default='tools/network-src')
    parser.add_argument('--cc', default=shutil.which('musl-gcc'))
    parser.add_argument('--musl-libc', default='/usr/lib/x86_64-linux-musl/libc.so')
    args = parser.parse_args()
    if not args.cc:raise SystemExit('musl-gcc is required (install musl-tools or pass --cc)')
    out = ROOT / args.output; out.mkdir(parents=True, exist_ok=True)
    source_root = ROOT / args.sources
    lock = json.loads((ROOT / 'network-sources.lock.json').read_text())
    source_data = {}
    for name, item in lock.items():
        path = source_root / name
        data = path.read_bytes()
        if hashlib.sha256(data).hexdigest() != item['sha256']:
            raise SystemExit(f'Pinned source hash mismatch: {name}')
        source_data[name] = data
    with tempfile.TemporaryDirectory(prefix='aurora-network-bootstrap-') as temp:
        work = Path(temp)
        compiler = shutil.which(args.cc) or args.cc
        compiler_version = subprocess.check_output([compiler, '--version'], text=True).splitlines()[0]
        env = dict(os.environ, CC=compiler, AR=shutil.which('ar') or 'ar',
                   RANLIB=shutil.which('ranlib') or 'ranlib', CFLAGS='-Os',
                   LDFLAGS='-static -Wl,-z,max-page-size=0x200000')

        mbed_archive = work / 'mbedtls.tar.bz2'; mbed_archive.write_bytes(source_data['mbedtls-3.6.7.tar.bz2'])
        with tarfile.open(mbed_archive, 'r:bz2') as archive: archive.extractall(work, filter='data')
        mbed = work / 'mbedtls-3.6.7'; subprocess.run(['make','-j2','GEN_FILES=','lib'],cwd=mbed,env=env,check=True)
        sysroot = work / 'netroot'; (sysroot/'lib').mkdir(parents=True); (sysroot/'include').mkdir()
        for library in (mbed/'library').glob('libmbed*.a'): shutil.copyfile(library,sysroot/'lib'/library.name)
        for name in ('mbedtls','psa'): shutil.copytree(mbed/'include'/name,sysroot/'include'/name)

        curl_archive = work / 'curl.tar.gz'; curl_archive.write_bytes(source_data['curl-8.22.0.tar.gz'])
        with tarfile.open(curl_archive, 'r:gz') as archive: archive.extractall(work, filter='data')
        curl = work / 'curl-8.22.0'; config_env=dict(env, CFLAGS='-Os',
            LDFLAGS='-static -Wl,-z,max-page-size=0x200000')
        options=['--prefix=/usr','--disable-shared','--enable-static',f'--with-mbedtls={sysroot}',
            '--disable-threaded-resolver','--disable-ipv6','--disable-unix-sockets','--disable-ldap','--disable-ldaps',
            '--disable-ftp','--disable-file','--disable-rtsp','--disable-dict','--disable-telnet','--disable-tftp',
            '--disable-pop3','--disable-imap','--disable-smtp','--disable-smb','--disable-gopher','--disable-mqtt',
            '--without-zlib','--without-brotli','--without-zstd','--without-libpsl','--without-libidn2',
            '--without-librtmp','--without-nghttp2','--without-libssh2','--with-ca-bundle=/etc/ssl/cert.pem','--with-ca-path=no']
        subprocess.run(['./configure',*options],cwd=curl,env=config_env,check=True)
        subprocess.run(['make','-j2'],cwd=curl,env=config_env,check=True)
        binary=curl/'src/curl'
        program_headers=subprocess.run(['readelf','-l',str(binary)],capture_output=True,text=True,check=True).stdout
        if '[Requesting program interpreter: /lib/ld-musl-x86_64.so.1]' not in program_headers:
            raise SystemExit('curl binary must use Aurora\'s existing musl loader')
        dynamic=subprocess.run(['readelf','-d',str(binary)],capture_output=True,text=True,check=True).stdout
        if any('Shared library:' in line and 'libc.so' not in line for line in dynamic.splitlines()):
            raise SystemExit('curl has an unexpected dynamic library dependency')

        entries={'bin/curl':binary.read_bytes(), 'etc/ssl/cert.pem':source_data['cacert.pem'],
                 'lib/ld-musl-x86_64.so.1':Path(args.musl_libc).read_bytes(),
                 'lib/libc.so':Path(args.musl_libc).read_bytes(),
                 'src/curl-8.22.0.tar.gz':source_data['curl-8.22.0.tar.gz'],
                 'src/mbedtls-3.6.7.tar.bz2':source_data['mbedtls-3.6.7.tar.bz2'],
                 'src/network-sources.lock.json':(ROOT/'network-sources.lock.json').read_bytes()}
        for library in (curl/'lib/.libs').glob('libcurl.a'): entries['lib/libcurl.a']=library.read_bytes()
        for library in sysroot.glob('lib/libmbed*.a'): entries['lib/'+library.name]=library.read_bytes()
        for include_root in (curl/'include/curl',sysroot/'include/mbedtls',sysroot/'include/psa'):
            rel = 'include/' + include_root.name
            copy_tree(include_root,entries,rel)
        archive=write_bundle(out,entries)
    manifest={'sha256':hashlib.sha256(archive.read_bytes()).hexdigest(),'sources':lock,
              'compiler':compiler_version+' (Linux host musl cross-compiler)',
              'musl_runtime':args.musl_libc,
              'recipe_sha256':hashlib.sha256((ROOT/'bootstrap-network.sh').read_bytes()).hexdigest(),
              'host_builder':'build-network-bootstrap-linux.py'}
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print('Built static curl 8.22.0 with Mbed TLS 3.6.7:',archive)


if __name__ == '__main__': main()
