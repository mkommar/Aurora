"""Compile and run tests/platform.c inside Aurora: signal semantics, job
control, demand paging, mremap, interval timers, build-critical syscalls and
batched storage. Uses an isolated disk copy and QMP port 4447. No host C
compiler is invoked. Kernel counters are sampled through QMP afterwards.
"""
import importlib.util,json,shutil,subprocess,time,struct,argparse
from pathlib import Path
from image_access import put_ext2_files
spec=importlib.util.spec_from_file_location('qmp','tools-qmp.py')
mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
parser=argparse.ArgumentParser();parser.add_argument('--disk',default='build/development.img');parser.add_argument('--ata',action='store_true',help='attach the development volume as the ATA primary slave instead of VirtIO');parser.add_argument('--cpus',type=int,default=2);parser.add_argument('--folder',default='build/platform-tests');parser.add_argument('--qmp-port',type=int,default=4447);parser.add_argument('--accel',choices=['tcg','whpx'],default='tcg');args=parser.parse_args()
folder=Path(args.folder);folder.mkdir(exist_ok=True)
shutil.copyfile('build/aurora.img',folder/'aurora.img');shutil.copyfile('build/kernel.elf',folder/'kernel.elf');shutil.copyfile(args.disk,folder/'development.img')
put_ext2_files(folder/'development.img',{'/work/platform.c':Path('tests/platform.c').read_bytes()})
q=None;process=None;results=[]
def log():return (folder/'serial.log').read_text(errors='replace')
def wait(predicate,seconds=120):
    deadline=time.monotonic()+seconds
    while time.monotonic()<deadline:
        if predicate():return
        if process and process.poll() is not None:raise AssertionError('QEMU exited: '+log()[-2000:])
        time.sleep(.1)
    raise AssertionError('Guest timeout:\n'+log()[-5000:])
def boot():
    global process,q
    process=subprocess.Popen(['tools/qemu/qemu-system-x86_64.exe','-machine','pc','-accel',args.accel,'-cpu','qemu64','-smp',str(args.cpus),'-m','1G',
        '-no-reboot','-vga','std','-drive',f'format=raw,file={folder}/aurora.img,if=ide,index=0',
        '-drive',f'format=raw,file={folder}/development.img,'+('if=ide,index=1' if args.ata else 'if=none,id=development'),
        *([] if args.ata else ['-device','virtio-blk-pci,drive=development,disable-modern=on']),
        '-serial',f'file:{folder}/serial.log','-net','none','-display','none',
        '-qmp',f'tcp:127.0.0.1:{args.qmp_port},server=on,wait=off'],creationflags=subprocess.CREATE_NO_WINDOW,stderr=(folder/'qemu-stderr.log').open('w'))
    deadline=time.monotonic()+45
    while True:
        try:q=mod.QMP(args.qmp_port);break
        except OSError:
            if process.poll() is not None:raise RuntimeError(f'QEMU exited before QMP connected: {process.returncode}: '+(folder/'qemu-stderr.log').read_text())
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
def command(text,seconds=180):
    print('GUEST:',text,flush=True);offset=len(log())
    for ch in text:
        if ch.isupper() or ch=='_':
            q.call('send-key',{'keys':[{'type':'qcode','data':'shift'},{'type':'qcode','data':'minus' if ch=='_' else ch.lower()}],'hold-time':30});time.sleep(.07)
        else:q.key({' ':'spc','-':'minus','.':'dot','/':'slash',',':'comma'}.get(ch,ch))
    q.key('ret');wait(lambda:'Application exited:' in log()[offset:],seconds);return log()[offset:]
def check(label,condition):
    assert condition,label+'\n'+log()[-6000:]
    results.append(label);print('PASS:',label,flush=True)
try:
    boot()
    out=command('gcc -O2 platform.c -o platform');check('Guest builds platform regression','Application exited: 0' in out)
    out=command('./platform',seconds=300)
    for label in ['nested signal delivery','fault signals with si_addr','sigaltstack','sigsuspend, sigpending, sigtimedwait and sigqueue','interval timers and pause','job control with WCONTINUED and waitid','demand paging and mremap','build-critical syscalls','bulk storage round trip']:
        check(label,'PASS '+label in out)
    check('Platform regression exit status','Application exited: 0' in out)
    nm=r'C:\Program Files\Unity\Hub\Editor\6000.4.0f1\Editor\Data\PlaybackEngines\AndroidPlayer\NDK\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-nm.exe'
    symbols={line.split()[2]:int(line.split()[0],16) for line in subprocess.check_output([nm,'-n',str(folder/'kernel.elf')],text=True).splitlines() if len(line.split())==3}
    counters={}
    for name in ['native_lazy_faults','native_lazy_commit_failures','native_signal_deliveries','native_fault_signals','virtio_requests','virtio_batches','virtio_batched_requests','virtio_max_batch','virtio_interrupts','virtio_suspensions','virtio_timeouts','ata_interrupts','ata_suspensions','ata_timeouts','ata_stale_interrupts','ata_polled','native_wait_blocks','native_wait_wakes']:
        target=(folder/(name+'.bin')).resolve();q.call('pmemsave',{'val':symbols[name],'size':8,'filename':str(target)})
        counters[name]=struct.unpack('<Q',target.read_bytes())[0]
    (folder/'counters.json').write_text(json.dumps(counters,indent=2),encoding='utf-8');print(json.dumps(counters,indent=2))
    check('Demand-zero pages were faulted in',counters['native_lazy_faults']>1000)
    check('Handlers ran through user-stack frames',counters['native_signal_deliveries']>=8 and counters['native_fault_signals']>=2)
    if args.ata:
        check('ATA transfers completed by interrupt',counters['ata_interrupts']>0 and counters['ata_suspensions']>0 and counters['ata_timeouts']==0)
    else:
        check('VirtIO batched multi-slot submissions',counters['virtio_batches']>0 and counters['virtio_max_batch']>=2 and counters['virtio_timeouts']==0)
        check('AuroraFS boot volume uses interrupt-driven ATA',counters['ata_timeouts']==0)
    out=command('sync');check('Filesystem sync','Application exited: 0' in out)
finally:
    stop()
    (folder/'results.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
