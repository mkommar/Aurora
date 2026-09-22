"""Independently inspect Aurora .deb outputs without extracting archive paths.

This is an artifact validator, not an installer or a replacement for dpkg.
"""
import argparse
import gzip
import hashlib
import io
import posixpath
from pathlib import Path, PurePosixPath
import tarfile


def inspect(path):
    raw=Path(path).read_bytes()
    if raw[:8]!=b'!<arch>\n': raise ValueError('Not a Debian ar archive')
    members={};offset=8
    while offset<len(raw):
        header=raw[offset:offset+60]
        if len(header)!=60 or header[58:]!=b'`\n': raise ValueError('Invalid ar header')
        name=header[:16].decode('ascii').strip().rstrip('/')
        size=int(header[48:58]);offset+=60
        if size<0 or offset+size>len(raw) or name in members: raise ValueError('Invalid ar member')
        if any(int(header[a:b].strip() or b'0') for a,b in ((16,28),(28,34),(34,40))):
            raise ValueError('Non-deterministic ar metadata')
        members[name]=raw[offset:offset+size];offset+=size+(size&1)
    if offset!=len(raw) or list(members)!=['debian-binary','control.tar.gz','data.tar.gz']:
        raise ValueError('Unexpected Debian archive layout')
    if members['debian-binary']!=b'2.0\n': raise ValueError('Unsupported Debian version')
    archives=[]
    for name in ('control.tar.gz','data.tar.gz'):
        data=members[name]
        if data[:3]!=b'\x1f\x8b\x08' or data[3]!=0 or data[4:8]!=bytes(4):
            raise ValueError('Non-deterministic gzip header')
        archives.append(tarfile.open(fileobj=io.BytesIO(gzip.decompress(data)),mode='r:'))
    control,data=archives
    try:
        fields={}
        text=control.extractfile('./control').read().decode()
        for line in text.splitlines():
            key,value=line.split(':',1)
            if key in fields: raise ValueError('Duplicate control field')
            fields[key]=value.strip()
        if fields.get('Architecture')!='musl-linux-amd64': raise ValueError('Wrong package ABI')
        if not fields.get('Package','').startswith('aurora-'): raise ValueError('Wrong package namespace')
        payload={}
        for member in data:
            name=member.name.rstrip('/')
            parts=PurePosixPath(name).parts
            if not parts or name.startswith('/') or '..' in parts or '.' in parts:
                raise ValueError('Unsafe payload path')
            if name not in ('opt','opt/aurora') and not name.startswith('opt/aurora/'):
                raise ValueError('Payload escapes candidate prefix')
            if name in payload: raise ValueError('Duplicate payload path')
            if not (member.isfile() or member.isdir() or member.issym() or member.islnk()):
                raise ValueError('Unsupported payload file type')
            if member.uid or member.gid or member.mtime!=1720000000:
                raise ValueError('Non-deterministic tar metadata')
            if member.issym() or member.islnk():
                target=member.linkname
                if member.issym() and not target.startswith('/'):
                    target=posixpath.join(posixpath.dirname(name),target)
                target=posixpath.normpath(target).lstrip('/')
                if target!='opt/aurora' and not target.startswith('opt/aurora/'):
                    raise ValueError('Link escapes candidate prefix')
            payload[name]=member
        expected={}
        for line in control.extractfile('./sha256sums').read().decode().splitlines():
            digest,name=line.split('  ',1)
            if name in expected: raise ValueError('Duplicate hash entry')
            expected[name]=digest
        files={name for name,member in payload.items() if member.isfile() or member.islnk()}
        if set(expected)!=files: raise ValueError('Hash manifest does not cover all files')
        for name,digest in expected.items():
            if hashlib.sha256(data.extractfile(payload[name]).read()).hexdigest()!=digest:
                raise ValueError('Payload checksum mismatch: '+name)
        listed=control.extractfile('./files').read().decode().splitlines()
        if len(set(listed))!=len(listed) or set(listed)!=set(payload):
            raise ValueError('Ownership manifest does not match payload')
        return {'package':fields['Package'],'version':fields['Version'],
            'files':len(files),'paths':len(payload),'sha256':hashlib.sha256(raw).hexdigest()}
    finally:
        control.close();data.close()


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('packages',nargs='+')
    args=parser.parse_args()
    for path in args.packages: print(path,inspect(path))
