"""Fetch exact networking inputs; reject changed content before using it."""
import hashlib,json,shutil,urllib.request
from pathlib import Path
root=Path('tools/network-src');root.mkdir(parents=True,exist_ok=True)
for name,entry in json.loads(Path('network-sources.lock.json').read_text()).items():
    target=root/name
    if not target.exists():
        vendored=Path('third_party/network-ca')/name
        data=vendored.read_bytes() if vendored.exists() else urllib.request.urlopen(entry['url'],timeout=120).read()
        if hashlib.sha256(data).hexdigest()!=entry['sha256']:raise RuntimeError(f'Hash mismatch: {name}')
        temporary=target.with_suffix(target.suffix+'.part');temporary.write_bytes(data);temporary.replace(target)
    if hashlib.sha256(target.read_bytes()).hexdigest()!=entry['sha256']:raise RuntimeError(f'Hash mismatch: {name}')
    print('Verified',name)
