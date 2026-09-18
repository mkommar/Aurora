"""End-to-end GNU shell and filesystem checks in an isolated Aurora VM."""
import argparse,importlib.util,json,shutil,subprocess,time,struct,os
from pathlib import Path
parser=argparse.ArgumentParser();parser.add_argument('--disk',default='build/development.img');parser.add_argument('--self-host',action='store_true');parser.add_argument('--self-host-only',action='store_true');parser.add_argument('--resume',action='store_true');parser.add_argument('--tools-only',action='store_true');parser.add_argument('--backport-only',action='store_true');parser.add_argument('--intx',action='store_true');parser.add_argument('--nm');parser.add_argument('--cpus',type=int,default=1);args=parser.parse_args()
folder=Path('build/development-intx-tests' if args.intx else 'build/development-tests');folder.mkdir(exist_ok=True)
shutil.copyfile('build/aurora.img',folder/'aurora.img')
if not args.resume:shutil.copyfile(args.disk,folder/'development.img')
spec=importlib.util.spec_from_file_location('qmp','tools-qmp.py');mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
process=None;q=None;checks=[];completed=False
def log():return (folder/'serial.log').read_text(errors='replace')
def wait(predicate,seconds=180):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        if predicate():return
        time.sleep(.1)
    raise AssertionError(log()[-7000:])
def type_line(text):
    plain={' ':'spc','-':'minus','.':'dot','/':'slash','=':'equal',"'":'apostrophe',';':'semicolon',',':'comma','[':'bracket_left',']':'bracket_right'}
    shifted={'|':'backslash','>':'dot','<':'comma','"':'apostrophe','$':'4','!':'1','(':'9',')':'0','_':'minus',':':'semicolon','&':'7','*':'8','?':'slash','{':'bracket_left','}':'bracket_right'}
    for ch in text:
        if ch in shifted or ch.isupper():
            key=shifted.get(ch,ch.lower());q.call('send-key',{'keys':[{'type':'qcode','data':'shift'},{'type':'qcode','data':key}],'hold-time':30});time.sleep(.07)
        else:q.key(plain.get(ch,ch))
    q.key('ret')
def command(text,seconds=180):
    offset=len(log());print('GUEST:',text,flush=True);type_line(text);wait(lambda:'Application exited:' in log()[offset:],seconds);return log()[offset:]
def check(name,condition):
    assert condition,name+'\n'+log()[-6000:];checks.append(name);print('PASS:',name,flush=True)
try:
    with (folder/'qemu-stderr.log').open('w') as stderr:
        process=subprocess.Popen(['tools/qemu/qemu-system-x86_64.exe','-machine','pc','-accel','tcg','-cpu','qemu64','-smp',str(args.cpus),'-m','1G','-display','none',
            '-drive',f'format=raw,file={folder}/aurora.img,if=ide,index=0','-drive',f'format=raw,file={folder}/development.img,if=none,id=development',
            '-device','virtio-blk-pci,drive=development,disable-modern=on'+(',vectors=0' if args.intx else ''),'-serial',f'file:{folder}/serial.log','-net','none',
            '-qmp','tcp:127.0.0.1:4447,server=on,wait=off'],creationflags=subprocess.CREATE_NO_WINDOW,stderr=stderr)
    end=time.monotonic()+45
    while q is None:
        try:q=mod.QMP(4447)
        except OSError:
            error=(folder/'qemu-stderr.log').read_text()
            if process.poll() is not None:
                if 'used by another process' in error and time.monotonic()<end:
                    time.sleep(1)
                    with (folder/'qemu-stderr.log').open('w') as stderr:process=subprocess.Popen(process.args,creationflags=subprocess.CREATE_NO_WINDOW,stderr=stderr)
                    continue
                raise RuntimeError(error)
            if time.monotonic()>end:raise RuntimeError(error)
            time.sleep(.2)
    wait(lambda:'desktop ready' in log());q.key('f2')
    check('GPT, ext2 and FAT32 mounted','GPT:' in log() and 'EXT2:' in log() and 'FAT32:' in log())
    if args.backport_only:
        out=command('bash backport-musl.sh',600)
        check('Pinned musl fork fix compiled and installed inside Aurora','AURORA_MUSL_FORK_BACKPORT_INSTALLED' in out and 'Application exited: 0' in out and '/cc1' in out)
        out=command('gcc -static -pthread foundations.c -o foundations')
        check('Thread regression linked against backported libc','Application exited: 0' in out)
        out=command('./foundations',180)
        check('Thread, shared-memory and COW regression exits successfully','Application exited: 0' in out)
        for marker in ['PASS POSIX threads:','PASS robust mutex owner death','PASS shared thread heap/cwd/umask','PASS process-shared pthread synchronization','PASS 192 MiB mapping and copy-on-write']:
            check(marker.removeprefix('PASS '),marker in out)
        out=command('./foundations leader-exit')
        check('Desktop waits for final thread and reports its exit status','THREAD_WORKER_FINISHED' in out and 'Application exited: 7' in out)
        q.capture('thread-runtime-tested');completed=True;raise SystemExit(0)
    if args.self_host_only:
        out=command('bash rebuild-make.sh',1800)
        check('GNU Make rebuilt and installed inside Aurora','AURORA_GNU_MAKE_REBUILT' in out and 'Application exited: 0' in out)
        q.capture('self-hosted-make');completed=True;raise SystemExit(0)
    if not args.tools_only:
        out=command('gcc -static filesystems.c -o filesystems');check('Filesystem regression compiled in Aurora','Application exited: 0' in out)
        out=command('./filesystems');check('ext2 and FAT32 file operations','PASS FAT32' in out and 'PASS ext2' in out and 'Application exited: 0' in out)
        out=command('bash --version');check('GNU Bash runs','GNU bash, version 5.2.37' in out and 'Application exited: 0' in out)
        out=command('make --version');check('GNU Make runs','GNU Make 4.4.1' in out and 'Application exited: 0' in out)
        out=command('make -B');check('GNU Make builds an application in Aurora','Application exited: 0' in out and '/cc1' in out)
        out=command('./made-demo');check('Make-built application executes','Compiled by GCC inside Aurora!' in out)
        for tool,version in [('sed','GNU sed'),('grep','GNU grep'),('gawk','GNU Awk'),('find','GNU findutils'),('tar','GNU tar'),('gzip','gzip'),('ar','GNU ar'),('ranlib','GNU ranlib')]:
            out=command(tool+' --version');check(tool+' is available',version in out and 'Application exited: 0' in out)
    offset=len(log());type_line('bash --noprofile --norc');wait(lambda:'NATIVE EXEC: /usr/bin/bash' in log()[offset:]);time.sleep(1)
    type_line('echo shell-ready');wait(lambda:'\nshell-ready' in log()[offset:])
    type_line("printf 'hello world' > /work/shell-output")
    type_line("read -r value < /work/shell-output; echo value=$value")
    wait(lambda:'\nvalue=hello world' in log()[offset:]);check('Interactive input, quoting, variables and redirection',True)
    type_line("echo piped-data | while read value; do echo $value; done")
    wait(lambda:'\npiped-data' in log()[offset:])
    type_line('echo pipeline-complete');wait(lambda:'\npipeline-complete' in log()[offset:]);check('Shell pipeline completes',True)
    type_line("echo alpha | sed s/alpha/beta/ | gawk '{print $1}' | grep beta || echo text-failed")
    wait(lambda:'\nbeta' in log()[offset:] or '\ntext-failed' in log()[offset:]);check('GNU sed, awk and grep process a pipeline','\nbeta' in log()[offset:])
    type_line('cp shell-output archive-input; tar -czf sample.tar.gz archive-input && rm archive-input && tar -xzf sample.tar.gz && test "$(cat shell-output)" = "$(cat archive-input)" && echo archive-ok || echo archive-failed')
    wait(lambda:'\narchive-ok' in log()[offset:] or '\narchive-failed' in log()[offset:]);check('GNU coreutils, tar and gzip archive round trip','\narchive-ok' in log()[offset:])
    type_line('exit');wait(lambda:'Application exited:' in log()[offset:]);check('Shell exits back to desktop','Application exited: 0' in log()[offset:])
    if args.self_host:
        out=command('bash rebuild-make.sh',1800)
        check('GNU Make rebuilt and installed inside Aurora','AURORA_GNU_MAKE_REBUILT' in out and '/cc1' in out and 'Application exited: 0' in out)
    nm=args.nm or shutil.which('llvm-nm') or str(Path(os.environ.get('AURORA_LLVM',r'C:\Program Files\Unity\Hub\Editor\6000.4.0f1\Editor\Data\PlaybackEngines\AndroidPlayer\NDK\toolchains\llvm\prebuilt\windows-x86_64\bin'))/'llvm-nm.exe')
    symbols={parts[2]:int(parts[0],16) for line in subprocess.check_output([nm,'-n','build/kernel.elf'],text=True).splitlines() if len(parts:=line.split())==3}
    counters={}
    q.call('stop')
    try:
        for name in ['virtio_interrupts','virtio_suspensions','virtio_timeouts','native_wait_blocks','native_wait_wakes']:
            probe=(folder/(name+'.bin')).resolve();q.call('pmemsave',{'val':symbols[name],'size':8,'filename':str(probe)})
            counters[name]=struct.unpack('<Q',probe.read_bytes())[0]
        probe=(folder/'kernel-stack-guards.bin').resolve();q.call('pmemsave',{'val':0x207000,'size':4096,'filename':str(probe)})
        entries=struct.unpack('<512Q',probe.read_bytes())
        check('All per-task kernel stacks have an unmapped guard',all(entries[256+i*16]==0 and entries[257+i*16]&3==3 for i in range(16)))
    finally:q.call('cont')
    (folder/'irq-counters.json').write_text(json.dumps(counters,indent=2))
    check('Disk requests suspend and complete through hardware interrupts',counters['virtio_interrupts']>0 and counters['virtio_suspensions']>0 and counters['virtio_timeouts']==0)
    check('Blocking waits sleep and wake',counters['native_wait_blocks']>0 and counters['native_wait_wakes']>0)
    check('Requested interrupt route active',('INTx' if args.intx else 'MSI-X') in log())
    check('No kernel panic or unexpected process faults','KERNEL PANIC' not in log() and 'FAULT isolated task=' not in log())
    q.capture('development-tested');completed=True
finally:
    if q:
        try:
            if process.poll() is None:
                type_line('sync');time.sleep(1)
            q.call('quit')
        finally:q.f.close();q.sock.close()
    if process:
        try:process.wait(timeout=15)
        except subprocess.TimeoutExpired:process.terminate();process.wait()
    (folder/'results.json').write_text(json.dumps({'success':completed,'passed':checks},indent=2))
print(len(checks),'development checks passed.')
