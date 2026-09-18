"""Stage pinned musl source and tests on a NEW COPY of Aurora's GPT development disk.
No compiler runs here. backport-musl.sh compiles and installs the fix in Aurora.
"""
import argparse,hashlib,json,shutil,tarfile,urllib.request
from image_access import put_ext2_files
from pathlib import Path

p=argparse.ArgumentParser();p.add_argument('--source',default='build/development.img');p.add_argument('--output',default='build/thread-runtime-candidate.img');args=p.parse_args()
target=Path(args.output);partial=target.with_suffix('.img.partial');source=Path(args.source)
if target.exists() or partial.exists():raise SystemExit('Refusing to overwrite an existing candidate')
archive=Path('tools/musl-backport/musl-1.2.2.tar.gz');archive.parent.mkdir(parents=True,exist_ok=True)
digest='9b969322012d796dc23dda27a35866034fa67d8fb67e0e2c45c913c3d43219dd'
url='https://musl.libc.org/releases/musl-1.2.2.tar.gz'
if not archive.exists():
    with urllib.request.urlopen(url,timeout=60) as response:archive.write_bytes(response.read())
if hashlib.sha256(archive.read_bytes()).hexdigest()!=digest:raise SystemExit('Pinned musl source checksum mismatch')
with tarfile.open(archive) as tar:
    original=tar.extractfile('musl-1.2.2/src/process/_Fork.c').read()
before=b'self->tid = __syscall(SYS_gettid);';after=b'self->tid = __syscall(SYS_set_tid_address, &__thread_list_lock);'
assert original.count(before)==1
fixed=original.replace(before,after)
shutil.copyfile(source,partial)
put_ext2_files(partial,{'/src/musl-1.2.2.tar.gz':archive.read_bytes(),'/work/musl-Fork-fixed.c':fixed,
    '/work/backport-musl.sh':Path('backport-musl.sh').read_text().encode(),
    '/work/musl-fork-abi.h':Path('tools-source/musl-fork-abi.h').read_bytes(),
    '/work/foundations.c':Path('tests/native-foundations.c').read_bytes()})
partial.rename(target)
manifest={'source_url':url,'source_sha256':digest,'original_fork_sha256':hashlib.sha256(original).hexdigest(),
          'patched_fork_sha256':hashlib.sha256(fixed).hexdigest(),'change':'Register __thread_list_lock through set_tid_address in the fork child.',
          'upstream_report':'https://www.openwall.com/lists/musl/2023/06/01/3','candidate':str(target),'compiled':False}
target.with_suffix('.sources.json').write_text(json.dumps(manifest,indent=2))
print('Staged sources on',target,'; compile with bash /work/backport-musl.sh inside Aurora.')
