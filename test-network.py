"""Exercise Aurora networking on disposable disks and loopback-only fixtures."""
import importlib.util,json,shutil,subprocess,time,struct,argparse,re
from pathlib import Path
spec=importlib.util.spec_from_file_location('qmp','tools-qmp.py')
mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
parser=argparse.ArgumentParser()
parser.add_argument('--disk',default='build/development.img')
parser.add_argument('--cpus',type=int,choices=range(1,9),default=1)
parser.add_argument('--resume',action='store_true')
parser.add_argument('--folder',default='build/network-tests')
parser.add_argument('--qmp-port',type=int,default=4450)
parser.add_argument('--script')
parser.add_argument('--intx',action='store_true')
parser.add_argument('--rebuild',action='store_true')
parser.add_argument('--accel',choices=['tcg','whpx'],default='tcg')
args=parser.parse_args();args.virtio=True
folder=Path(args.folder);folder.mkdir(exist_ok=True)
shutil.copyfile('build/aurora.img',folder/'aurora.img');shutil.copyfile('build/kernel.elf',folder/'kernel.elf')
if not args.resume:shutil.copyfile(args.disk,folder/'toolchain.img')
from image_access import put_ext2_files
from network_artifacts import files as network_files
files=network_files()
from network_test_fixture import start
servers,fixture_files=start(folder)
files.update(fixture_files)
if args.script:files['/work/net-test.sh']=Path(args.script).read_bytes()
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
        '-serial',f'file:{folder}/serial.log','-netdev','user,id=net0','-device','virtio-net-pci,netdev=net0,disable-modern=on'+(',vectors=0' if args.intx else ''),'-object','rng-builtin,id=rng0','-device','virtio-rng-pci,rng=rng0,disable-modern=on','-display','none',
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
    if args.rebuild:
        out=command('bash /work/rebuild-network.sh',seconds=5400)
        print(out[-6000:],flush=True)
        check('curl and Mbed TLS rebuilt inside Aurora','AURORA_NETWORK_REBUILT_INSIDE_AURORA' in out and 'Application exited: 0' in out)
    out=command('bash /work/net-test.sh',seconds=360)
    print(out,flush=True)
    for marker in re.findall(r'^(?:PASS[^\r\n]*|AURORA_NETWORK_ABI_PASS)$',out,re.M):results.append(marker)
    check('network script completes','AURORA_NETWORK_TEST_COMPLETE' in out and 'Application exited: 0' in out)
    (folder/'results.json').write_text(json.dumps({'checks':results,'cpus':args.cpus,'log':'serial.log'},indent=2))
finally:
    if q:
        try:command('sync',seconds=30)
        except Exception:pass
    stop()
    for server in servers:server.shutdown();server.server_close()
