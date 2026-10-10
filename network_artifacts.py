"""Verified bootstrap payload, shared by staging and guest validation."""
import hashlib,json,tarfile
from pathlib import Path,PurePosixPath

def files():
    root=Path('tools/network-bootstrap')
    archive=root/'network-bootstrap.tar.gz'
    manifest=json.loads((root/'manifest.json').read_text())
    lock=json.loads(Path('network-sources.lock.json').read_text())
    if manifest.get('sources') != lock:
        raise ValueError('Bootstrap manifest source lock mismatch')
    if manifest.get('recipe_sha256') != hashlib.sha256(Path('bootstrap-network.sh').read_bytes()).hexdigest():
        raise ValueError('Bootstrap recipe hash mismatch')
    if hashlib.sha256(archive.read_bytes()).hexdigest()!=manifest['sha256']:raise ValueError('Bootstrap archive hash mismatch')
    result={}
    with tarfile.open(archive) as source:
        for member in source:
            if not member.isfile():
                raise ValueError('Bootstrap archive contains a non-file: '+member.name)
            path=PurePosixPath(member.name)
            if (not path.parts or path.is_absolute() or '..' in path.parts or
                    path.parts[0] not in ('bin','lib','include','src','etc') or
                    str(path) != member.name or str(path) in result):
                raise ValueError('Unexpected bootstrap path: '+member.name)
            result['/'+str(path)]=source.extractfile(member).read()
    for required in ('/etc/ssl/cert.pem','/src/curl-8.22.0.tar.gz','/src/mbedtls-3.6.7.tar.bz2'):
        if required not in result:raise ValueError('Bootstrap archive missing '+required)
    for name in ('curl-8.22.0.tar.gz','mbedtls-3.6.7.tar.bz2'):
        if hashlib.sha256(result['/src/'+name]).hexdigest()!=lock[name]['sha256']:raise ValueError('Source hash mismatch: '+name)
    if hashlib.sha256(result['/etc/ssl/cert.pem']).hexdigest()!=lock['cacert.pem']['sha256']:raise ValueError('CA bundle hash mismatch')
    result.update({'/etc/resolv.conf':b'nameserver 10.0.2.3\noptions timeout:2 attempts:2\n',
                   '/work/rebuild-network.sh':Path('rebuild-network.sh').read_bytes(),
                   '/work/archive-libcurl.sh':Path('archive-libcurl.sh').read_bytes(),
                   '/work/http-demo.c':Path('tests/http-demo.c').read_bytes(),
                   '/src/network-sources.lock.json':Path('network-sources.lock.json').read_bytes()})
    return result
