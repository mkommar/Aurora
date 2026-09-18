"""Stage the guest-built runtime onto a copy of the latest development disk.
Never replaces the user's disk. The candidate still needs regression/fsck checks.
"""
import argparse,hashlib,importlib.util,json,shutil,subprocess,time
from pathlib import Path
from image_access import put_ext2_files

p=argparse.ArgumentParser();p.add_argument('--disk',default='build/development.img');p.add_argument('--folder',default='build/smp-release');args=p.parse_args()
folder=Path(args.folder);folder.mkdir(exist_ok=True);candidate=folder/'development.img'
if candidate.exists():raise SystemExit('Candidate already exists; choose a new --folder')
def digest(path):
    with Path(path).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
source_hash=digest(args.disk);shutil.copyfile(args.disk,candidate)
assert digest(candidate)==source_hash==digest(args.disk),'Source disk changed during copy'
proof=Path('build/musl-shared-build');libc=(proof/'libc.so').read_bytes();libgcc=(proof/'libgcc_s.so.1').read_bytes()
expected=json.loads((proof/'runtime-sha256.json').read_text())['/lib/libc.so']
assert hashlib.sha256(libc).hexdigest()==expected
put_ext2_files(candidate,{'/lib/libc.so':libc,'/lib/ld-musl-x86_64.so.1':libc,
    '/lib/libgcc_s.so':libgcc,'/lib/libgcc_s.so.1':libgcc,'/work/build-shared-musl.sh':Path('build-shared-musl.sh').read_bytes()})
shutil.copyfile('build/aurora.img',folder/'aurora.img')
spec=importlib.util.spec_from_file_location('qmp','tools-qmp.py');mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
process=None;q=None;results=[]
def log():return (folder/'serial.log').read_text(errors='replace')
def wait(check,seconds=120):
    until=time.monotonic()+seconds
    while time.monotonic()<until:
        if check():return
        if process.poll() is not None:raise RuntimeError('Guest exited: '+log()[-2000:])
        time.sleep(.1)
    raise RuntimeError('Guest timeout: '+log()[-2000:])
def command(text):
    start=len(log());print('GUEST:',text,flush=True)
    for c in text:
        if c=='_':q.call('send-key',{'keys':[{'type':'qcode','data':'shift'},{'type':'qcode','data':'minus'}],'hold-time':30});time.sleep(.07)
        else:q.key({' ':'spc','-':'minus','.':'dot','/':'slash'}.get(c,c))
    q.key('ret');wait(lambda:'Application exited:' in log()[start:]);out=log()[start:]
    assert 'Application exited: 0' in out,out
    results.append(text);return out
try:
    process=subprocess.Popen(['tools/qemu/qemu-system-x86_64.exe','-machine','pc','-accel','tcg','-cpu','qemu64','-smp','4','-m','1G','-no-reboot','-vga','std',
        '-drive',f'format=raw,file={folder}/aurora.img,if=ide,index=0','-drive',f'format=raw,file={candidate},if=none,id=development',
        '-device','virtio-blk-pci,drive=development,disable-modern=on','-serial',f'file:{folder}/serial.log','-net','none','-display','none',
        '-qmp','tcp:127.0.0.1:4453,server=on,wait=off'],creationflags=subprocess.CREATE_NO_WINDOW,stderr=(folder/'qemu-stderr.log').open('w'))
    for _ in range(100):
        try:q=mod.QMP(4453);break
        except OSError:time.sleep(.1)
    if q is None:raise RuntimeError('QMP unavailable')
    wait(lambda:'desktop ready' in log());q.key('f2')
    command('chmod 755 /lib/libc.so');command('ln -sf libc.so /lib/ld-musl-x86_64.so.1')
    assert expected in command('sha256sum /lib/libc.so')
    command('sync')
finally:
    if q:
        try:
            if process.poll() is None:command('sync')
        finally:
            q.call('quit');q.f.close();q.sock.close()
    if process:process.wait(timeout=15)
manifest={'source_image':str(Path(args.disk).resolve()),'source_sha256':source_hash,'candidate_sha256':digest(candidate),
    'libc_sha256':expected,'libgcc_sha256':hashlib.sha256(libgcc).hexdigest(),'guest_install_commands':results}
(folder/'staging.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
print('Staged candidate:',candidate)
