"""Run GCC, compilation, linking and generated applications inside Aurora.
Uses isolated disk copies and QMP port 4446. No host C compiler is invoked.
"""
import importlib.util,json,shutil,subprocess,time,struct,argparse
from pathlib import Path
spec=importlib.util.spec_from_file_location('qmp','tools-qmp.py')
mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
parser=argparse.ArgumentParser();parser.add_argument('--disk',default='build/development.img');parser.add_argument('--virtio',action='store_true');parser.add_argument('--foundations-only',action='store_true');parser.add_argument('--cpus',type=int,default=1);parser.add_argument('--qmp-port',type=int,default=4446);args=parser.parse_args();args.virtio=args.virtio or args.disk!='build/toolchain.img'
folder=Path('build/ext2-tests' if args.disk!='build/toolchain.img' else 'build/native-tests');folder.mkdir(exist_ok=True)
shutil.copyfile('build/aurora.img',folder/'aurora.img');shutil.copyfile(args.disk,folder/'toolchain.img')
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
    put_ext2_files(folder/'toolchain.img',{'/work/foundations.c':Path('tests/native-foundations.c').read_bytes(),'/work/smp.c':Path('tests/native-smp.c').read_bytes()})
q=None;process=None;results=[]
def log():return (folder/'serial.log').read_text(errors='replace')
def wait(predicate,seconds=120):
    deadline=time.monotonic()+seconds
    while time.monotonic()<deadline:
        if predicate():return
        time.sleep(.1)
    if q:
        q.call('pmemsave',{'val':0,'size':0x70000,'filename':str((folder/'timeout-kernel.bin').resolve())})
    raise AssertionError('Guest timeout:\n'+log()[-5000:])
def boot():
    global process,q
    process=subprocess.Popen(['tools/qemu/qemu-system-x86_64.exe','-machine','pc','-accel','tcg','-cpu','qemu64','-smp',str(args.cpus),'-m','1G',
        '-vga','std','-drive',f'format=raw,file={folder}/aurora.img,if=ide,index=0',
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
        finally:q.f.close();q.sock.close();q=None
    if process:process.wait(timeout=15);process=None
def command(text,application=True):
    print('GUEST:',text,flush=True);offset=len(log())
    for ch in text:q.key({' ':'spc','-':'minus','.':'dot','/':'slash'}.get(ch,ch))
    q.key('ret')
    if application:wait(lambda:'Application exited:' in log()[offset:])
    else:time.sleep(.3)
    return log()[offset:]
def check(label,condition):
    assert condition,label+'\n'+log()[-5000:]
    results.append(label);print('PASS:',label,flush=True)
try:
    boot()
    if not args.foundations_only:
        out=command('gcc --version')
        check('GCC driver runs in Aurora','gcc (GCC) 11.2.1' in out and 'Application exited: 0' in out)
        out=command('gcc -static demo.c -o demo')
        check('Guest GCC, cc1, assembler and linker compile demo','Application exited: 0' in out and '/cc1' in out and '/bin/as' in out and '/bin/ld' in out)
        out=command('./demo');check('Guest-built executable runs','Compiled by GCC inside Aurora!' in out and 'Application exited: 0' in out)
        out=command('gcc -static compute.c -o compute');check('Second compilation succeeds','Application exited: 0' in out)
        out=command('./compute');check('Generated code, malloc and printf work','sum=499500' in out and 'Application exited: 0' in out)
        out=command('gcc -static math.c -o math');check('Floating-point application compiles','Application exited: 0' in out)
        out=command('./math');check('Floating-point/SIMD context works','fp=3.0' in out and 'Application exited: 0' in out)
        out=command('gcc -static broken.c -o broken');check('Compiler diagnoses invalid source and returns failure','error:' in out and 'Application exited: 1' in out)
        out=command('gcc -static demo.c -o demo');check('Toolchain recovers after compilation failure','Application exited: 0' in out)
        check('No kernel or process faults during toolchain test','FAULT isolated task=' not in log() and 'KERNEL PANIC' not in log())
    out=command('gcc -static -pthread foundations.c -o foundations');check('Foundation regression compiles inside Aurora','Application exited: 0' in out)
    out=command('./foundations');check('Shared descriptors, fork, pipes and reclamation','Application exited: 0' in out and 'PASS open-description reclamation' in out and 'PASS large pipe transfer' in out)
    for marker in ['PASS poll/select readiness','PASS shared anonymous mappings and futex','PASS POSIX threads:',
                   'PASS robust mutex owner death','PASS shared thread heap/cwd/umask',
                   'PASS process-shared pthread synchronization','PASS 192 MiB mapping and copy-on-write']:
        check(marker.removeprefix('PASS '),marker in out)
    if args.cpus>1 and magic!=b'AURDEV01':
        out=command('gcc -static -pthread smp.c -o smp');check('SMP regression compiles inside Aurora','Application exited: 0' in out)
        out=command('./smp');check('Pinned threads, GS isolation, remote protection changes and COW','PASS SMP pinned pthreads' in out and 'PASS SMP fork/COW' in out and 'PASS remote CPU loses stale write permission' in out and 'Application exited: 0' in out)
    out=command('./foundations leader-exit');check('Desktop waits for final thread and preserves exit status','THREAD_WORKER_FINISHED' in out and 'Application exited: 7' in out)
    (folder/'foundations-serial.log').write_text(log())
    out=command('sync');check('Filesystem flush succeeds before reboot','Application exited: 0' in out)
    stop();boot();out=command('hello aurora' if args.foundations_only else './demo');check('SDK application runs after restart' if args.foundations_only else 'Guest-built executable survives VM restart',('Hello, aurora!' if args.foundations_only else 'Compiled by GCC inside Aurora!') in out)
    out=command('hello aurora');check('Original Aurora SDK applications still run','Hello, aurora!' in out)
    q.capture('native-gcc-tested')
    out=command('sync');check('Filesystem flush succeeds before shutdown','Application exited: 0' in out)
finally:stop()
(folder/'results.json').write_text(json.dumps({'passed':results},indent=2))
print(f'{len(results)} native GCC checks passed.')
