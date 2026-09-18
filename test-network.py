"""Run GCC, compilation, linking and generated applications inside Aurora.
Uses isolated disk copies and QMP port 4446. No host C compiler is invoked.
"""
import importlib.util,json,shutil,subprocess,time,struct,argparse,re
from pathlib import Path
spec=importlib.util.spec_from_file_location('qmp','tools-qmp.py')
mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
parser=argparse.ArgumentParser();parser.add_argument('--disk',default='build/development.img');parser.add_argument('--virtio',action='store_true');parser.add_argument('--foundations-only',action='store_true');parser.add_argument('--cpus',type=int,default=1);parser.add_argument('--rebuild-musl',action='store_true');parser.add_argument('--continue-musl',action='store_true');parser.add_argument('--resume',action='store_true');parser.add_argument('--folder',default='build/network-tests');parser.add_argument('--qmp-port',type=int,default=4450);parser.add_argument('--accel',choices=['tcg','whpx'],default='tcg');args=parser.parse_args();args.virtio=args.virtio or args.disk!='build/toolchain.img'
folder=Path(args.folder);folder.mkdir(exist_ok=True)
shutil.copyfile('build/aurora.img',folder/'aurora.img');shutil.copyfile('build/kernel.elf',folder/'kernel.elf')
if not args.resume:shutil.copyfile(args.disk,folder/'toolchain.img')
from image_access import put_ext2_files
import tarfile
files={}
with tarfile.open('tools/network-bootstrap/network-bootstrap.tar.gz') as archive:
    for entry in archive:
        if entry.isfile():files['/'+entry.name.removeprefix('./')]=archive.extractfile(entry).read()
files['/etc/resolv.conf']=b'nameserver 10.0.2.3\noptions timeout:2 attempts:2\n'
files['/etc/hosts']=b'127.0.0.1 localhost\n'
files['/work/net-test.sh']=b'#!/bin/sh\n/bin/curl --version\n/bin/curl -v --max-time 20 http://example.com/\n/bin/curl -v --max-time 30 https://example.com/\necho NETWORK_SCRIPT_DONE\n'
put_ext2_files(folder/'toolchain.img',files)
q=None;process=None;results=[]
def log():return (folder/'serial.log').read_text(errors='replace')
def wait(predicate,seconds=120):
    deadline=time.monotonic()+seconds
    while time.monotonic()<deadline:
        if predicate():return
        if process and process.poll() is not None:raise AssertionError('QEMU exited: '+log()[-2000:])
        time.sleep(.1)
    if q:
        q.call('pmemsave',{'val':0,'size':0x70000,'filename':str((folder/'timeout-kernel.bin').resolve())})
    raise AssertionError('Guest timeout:\n'+log()[-5000:])
def boot():
    global process,q
    process=subprocess.Popen(['tools/qemu/qemu-system-x86_64.exe','-machine','pc','-accel',args.accel,'-cpu','qemu64','-smp',str(args.cpus),'-m','1G',
        '-no-reboot','-vga','std','-drive',f'format=raw,file={folder}/aurora.img,if=ide,index=0',
        '-drive',f'format=raw,file={folder}/toolchain.img,'+('if=none,id=development' if args.virtio else 'if=ide,index=1'),
        *(['-device','virtio-blk-pci,drive=development,disable-modern=on'] if args.virtio else []),
        '-serial',f'file:{folder}/serial.log','-netdev','user,id=net0','-device','virtio-net-pci,netdev=net0,disable-modern=on','-object','rng-builtin,id=rng0','-device','virtio-rng-pci,rng=rng0,disable-modern=on','-display','none',
        '-qmp',f'tcp:127.0.0.1:{args.qmp_port},server=on,wait=off'],creationflags=subprocess.CREATE_NO_WINDOW,stderr=(folder/'qemu-stderr.log').open('w'))
    deadline=time.monotonic()+45
    while True:
        try:q=mod.QMP(args.qmp_port);break
        except OSError:
            if process.poll() is not None:
                error=(folder/'qemu-stderr.log').read_text()
                if 'used by another process' in error and time.monotonic()<deadline:
                    time.sleep(1)
                    process=subprocess.Popen(process.args,creationflags=subprocess.CREATE_NO_WINDOW,stderr=(folder/'qemu-stderr.log').open('w'))
                    continue
                raise RuntimeError(f'QEMU exited before QMP connected: {process.returncode}: '+error)
            if time.monotonic()>deadline:raise
            time.sleep(.1)
    wait(lambda:'desktop ready' in log());q.key('f2')
def stop():
    global q,process
    if q:
        try:q.call('quit')
        except OSError:pass
        try:q.f.close()
        except OSError:pass
        q.sock.close();q=None
    if process:process.wait(timeout=15);process=None
def command(text,application=True,seconds=120):
    print('GUEST:',text,flush=True);offset=len(log())
    for ch in text:
        if ch.isupper() or ch=='_':
            q.call('send-key',{'keys':[{'type':'qcode','data':'shift'},{'type':'qcode','data':'minus' if ch=='_' else ch.lower()}],'hold-time':30});time.sleep(.07)
        else:q.key({' ':'spc','-':'minus','.':'dot','/':'slash',',':'comma'}.get(ch,ch))
    q.key('ret')
    if application:wait(lambda:'Application exited:' in log()[offset:],seconds)
    else:time.sleep(.3)
    return log()[offset:]
def check(label,condition):
    assert condition,label+'\n'+log()[-5000:]
    results.append(label);print('PASS:',label,flush=True)
try:
    boot()
    wait(lambda:'NET: DHCP' in log(),60)
    out=command('chmod 755 /bin/curl');check('curl executable', 'Application exited: 0' in out)
    out=command('bash /work/net-test.sh',seconds=100)
    print(out,flush=True)
    check('network script completes','NETWORK_SCRIPT_DONE' in out)
finally:
    stop()
