"""Run GCC, compilation, linking and generated applications inside Aurora.
Uses isolated disk copies and QMP port 4446. No host C compiler is invoked.
"""
import importlib.util,json,shutil,subprocess,time,struct,argparse
from pathlib import Path
spec=importlib.util.spec_from_file_location('qmp','tools-qmp.py')
mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
parser=argparse.ArgumentParser();parser.add_argument('--disk',default='build/development.img');parser.add_argument('--virtio',action='store_true');parser.add_argument('--foundations-only',action='store_true');parser.add_argument('--cpus',type=int,default=1);parser.add_argument('--rebuild-musl',action='store_true');parser.add_argument('--continue-musl',action='store_true');parser.add_argument('--resume',action='store_true');parser.add_argument('--folder',default='build/dynamic-tests');parser.add_argument('--qmp-port',type=int,default=4446);parser.add_argument('--accel',choices=['tcg','whpx'],default='tcg');args=parser.parse_args();args.virtio=args.virtio or args.disk!='build/toolchain.img'
folder=Path(args.folder);folder.mkdir(exist_ok=True)
shutil.copyfile('build/aurora.img',folder/'aurora.img');shutil.copyfile('build/kernel.elf',folder/'kernel.elf')
if not args.resume:shutil.copyfile(args.disk,folder/'toolchain.img')
# Add the regression source to the isolated test disk, preserving the user's disk.
with (folder/'toolchain.img').open('r+b') as disk:
    magic,count,sector=struct.unpack('<8sII',disk.read(16))
    if magic==b'AURDEV01':
        assert count<4096
        source=Path('tests/native-foundations.c').read_bytes();capacity=(len(source)+511)//512
        assert sector+capacity<1048576
        disk.seek(sector*512);disk.write(source)
        disk.seek((count+1)*512);disk.write(struct.pack('<256sQQII232x',b'/work/foundations.c',sector,len(source),capacity,1))
        disk.seek(0);disk.write(struct.pack('<8sII',magic,count+1,sector+capacity))
if magic!=b'AURDEV01':
    from image_access import put_ext2_files

import tarfile,hashlib
archive_path=Path('tools/gcc-native/x86_64-linux-musl-native.tgz')
assert hashlib.sha512(archive_path.read_bytes()).hexdigest()=='44d441ad9aa11a06feddf3daa4c9f53ad7d9ca37af1f5a61379aca07793703d179410cea723c1b7fca94c4de19a321228bdb3656bc5cbdb5e3bea8e2d6dac6c7'
with tarfile.open('tools/gcc-native/x86_64-linux-musl-native.tgz') as archive:
    libgcc=archive.extractfile('x86_64-linux-musl-native/lib/libgcc_s.so.1').read()
    libc=archive.extractfile('x86_64-linux-musl-native/lib/libc.so').read()
files={'/lib/libgcc_s.so':libgcc,'/lib/libgcc_s.so.1':libgcc,
    '/work/lib.c':Path('tests/dynamic-library.c').read_bytes(),
    '/work/dyn.c':Path('tests/dynamic-posix.c').read_bytes(),'/work/smp.c':Path('tests/native-smp.c').read_bytes(),'/work/plugin.c':Path('tests/dynamic-plugin.c').read_bytes(),'/work/build-shared-musl.sh':Path('build-shared-musl.sh').read_bytes()}
if not args.resume:files.update({'/lib/libc.so':libc,'/lib/ld-musl-x86_64.so.1':libc})
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
        '-serial',f'file:{folder}/serial.log','-net','none','-display','none',
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
    if args.rebuild_musl or args.continue_musl:
        out=command('bash /work/build-shared-musl.sh'+(' resume' if args.continue_musl else ''),seconds=5400);check('Pinned musl shared runtime rebuilt inside Aurora','AURORA_SHARED_MUSL_BUILT_FROM_PINNED_SOURCE' in out and 'Application exited: 0' in out)
    out=command('chmod 755 /lib/ld-musl-x86_64.so.1');check('Interpreter executable permission','Application exited: 0' in out)
    out=command('gcc -shared -fPIC lib.c -o libprobe.so');check('Guest builds shared library','Application exited: 0' in out)
    out=command('gcc -shared -fPIC plugin.c -o plugin.so');check('Guest builds runtime-loaded plugin','Application exited: 0' in out)
    out=command('gcc -fPIE -pie -pthread dyn.c ./libprobe.so -o dyn');check('Guest builds dynamically linked PIE','Application exited: 0' in out)
    out=command('./dyn');check('Dynamic linker, TLS and POSIX regression','PASS dynamic PIE' in out and 'PASS POSIX groups' in out and 'PASS malformed ELF' in out and 'PASS patched dynamic fork' in out and 'Application exited: 0' in out)
    if args.cpus>1:
        out=command('gcc -static -O2 -pthread smp.c -o smp');check('Guest builds SMP regression','Application exited: 0' in out)
        out=command('./smp');check('Multicore pthreads and COW','PASS SMP pinned pthreads' in out and 'PASS SMP fork/COW' in out and 'Application exited: 0' in out)
    nm=r'C:\Program Files\Unity\Hub\Editor\6000.4.0f1\Editor\Data\PlaybackEngines\AndroidPlayer\NDK\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-nm.exe'
    symbols={line.split()[2]:int(line.split()[0],16) for line in subprocess.check_output([nm,'-n',str(folder/'kernel.elf')],text=True).splitlines() if len(line.split())==3}
    counters={}
    for name,count,width in [('cpu_online',1,4),('cpu_user_returns',8,8),('cpu_rendezvous',8,8),('cpu_fast_calls',8,8),('cpu_parallel_service_calls',1,8)]:
        target=(folder/(name+'.bin')).resolve();q.call('pmemsave',{'val':symbols[name],'size':count*width,'filename':str(target)})
        counters[name]=list(struct.unpack('<'+('I' if width==4 else 'Q')*count,target.read_bytes()))
    (folder/'cpu-counters.json').write_text(json.dumps(counters,indent=2),encoding='utf-8')
    check('Requested CPUs online',counters['cpu_online'][0]==args.cpus)
    if args.cpus>1:
        check('Application execution and rendezvous on additional CPUs',sum(counters['cpu_user_returns'][1:])>0 and sum(counters['cpu_rendezvous'][1:])>0)
        check('Service kernel operations overlap native kernel operations',counters['cpu_parallel_service_calls'][0]>0)
    out=command('sync');check('Filesystem sync' ,'Application exited: 0' in out)
finally:
    if q and process and process.poll() is None:
        try:command('sync')
        except Exception:pass
    stop()
    (folder/'results.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
