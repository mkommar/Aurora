"""Filesystem correctness and recovery inside Aurora, on an isolated copy of the
development disk (QMP port 4448, no visible window, no host C compiler):

1. Build tools-source/fsck-aurora.c, tests/filesystem.c and
   tests/interrupted-writes.c with the in-guest GCC; the checker must report
   the fresh copy clean; the semantics regression must pass; legacy and native
   file APIs must see one namespace (AuroraFS as /aurorafs, /work fallbacks).
2. Reboot after `sync` and confirm the ext2 superblock was left clean.
3. Run the interrupted-write workload, cut power mid-stream, reboot: the
   kernel must report the unclean stop, reclaim both parked crash orphans, the
   fsync'd file must hash correctly and fsck-aurora must find no structural
   damage.
4. Damage the primary GPT, then the backup GPT, then both, on the host between
   boots: the kernel must recover from the surviving copy and rewrite the
   damaged one; both copies must validate afterwards.
5. Repeat the checker over the ATA attachment (IDENTIFY sizes the raw device).
"""
import argparse,hashlib,importlib.util,shutil,struct,subprocess,time,zlib
from pathlib import Path
from image_access import put_ext2_files
spec=importlib.util.spec_from_file_location('qmp','tools-qmp.py')
mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
parser=argparse.ArgumentParser();parser.add_argument('--disk',default='build/development.img');parser.add_argument('--folder',default='build/filesystem-tests');parser.add_argument('--qmp-port',type=int,default=4448);parser.add_argument('--accel',choices=['tcg','whpx'],default='tcg');parser.add_argument('--cpus',type=int,default=2)
parser.add_argument('--nm',default=r'C:\Program Files\Unity\Hub\Editor\6000.4.0f1\Editor\Data\PlaybackEngines\AndroidPlayer\NDK\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-nm.exe');args=parser.parse_args()
folder=Path(args.folder);folder.mkdir(exist_ok=True)
shutil.copyfile('build/aurora.img',folder/'aurora.img');shutil.copyfile('build/kernel.elf',folder/'kernel.elf');shutil.copyfile('build/desktop.elf',folder/'desktop.elf');shutil.copyfile(args.disk,folder/'development.img')
disk=folder/'development.img';disk.chmod(0o666)
bridge='''set -e
echo native-side > /aurorafs/bridge.txt
ls /aurorafs
cat /aurorafs/notes.txt
rm /aurorafs/notes.txt
echo work-notes > /work/notes.txt
echo BRIDGE_DONE
'''
verify='''cat /work/fs-check.txt
ls -a / /exchange | grep -c aurora-orphan || true
sha256sum /work/iw-committed.bin
echo VERIFY_DONE
'''
put_ext2_files(disk,{'/work/fsck-aurora.c':Path('tools-source/fsck-aurora.c').read_bytes(),'/work/filesystem.c':Path('tests/filesystem.c').read_bytes(),
    '/work/interrupted-writes.c':Path('tests/interrupted-writes.c').read_bytes(),'/work/fs-bridge.sh':bridge.encode(),'/work/fs-verify.sh':verify.encode(),
    '/work/hellowork':Path('build/apps/hello.elf').read_bytes()})
q=None;process=None;results=[];ata=False
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
    for attempt in range(20):
        process=subprocess.Popen(['tools/qemu/qemu-system-x86_64.exe','-machine','pc','-accel',args.accel,'-cpu','qemu64','-smp',str(args.cpus),'-m','1G',
            '-no-reboot','-vga','std','-drive',f'format=raw,file={folder}/aurora.img,if=ide,index=0',
            '-drive',f'format=raw,file={disk},'+('if=ide,index=1' if ata else 'if=none,id=development'),
            *([] if ata else ['-device','virtio-blk-pci,drive=development,disable-modern=on']),
            '-serial',f'file:{folder}/serial.log','-net','none','-display','none',
            '-qmp',f'tcp:127.0.0.1:{args.qmp_port},server=on,wait=off'],creationflags=subprocess.CREATE_NO_WINDOW,stderr=(folder/'qemu-stderr.log').open('w'))
        deadline=time.monotonic()+45
        while True:
            try:q=mod.QMP(args.qmp_port);break
            except OSError:
                if process.poll() is not None:
                    stderr=(folder/'qemu-stderr.log').read_text()
                    # The image lives under a synced folder; a background indexer may hold it briefly after a host write.
                    if 'being used by another process' in stderr and attempt<19:process=None;time.sleep(.5);break
                    raise RuntimeError(f'QEMU exited before QMP connected: {process.returncode}: '+stderr)
                if time.monotonic()>deadline:raise
                time.sleep(.1)
        if process:break
    wait(lambda:'desktop ready' in log());q.key('f2')
def stop(hard=False):
    """hard=True cuts power without any guest cooperation (QMP quit is immediate)."""
    global q,process
    if q:
        try:q.call('quit')
        except OSError:pass
        try:q.f.close()
        except OSError:pass
        q.sock.close();q=None
    if process:process.wait(timeout=15);process=None
    time.sleep(.5)
SHIFTED={'"':'apostrophe','>':'dot','|':'backslash','*':'8','$':'4','(':'9',')':'0','&':'7','!':'1','~':'grave_accent','{':'bracket_left','}':'bracket_right',':':'semicolon','+':'equal','#':'3','%':'5','^':'6','@':'2','?':'slash','<':'comma','_':'minus'}
PLAIN={' ':'spc','-':'minus','.':'dot','/':'slash',',':'comma',"'":'apostrophe',';':'semicolon','=':'equal','[':'bracket_left',']':'bracket_right','\\':'backslash','`':'grave_accent'}
def type_text(text):
    for ch in text:
        if ch.isupper() or ch in SHIFTED:
            q.call('send-key',{'keys':[{'type':'qcode','data':'shift'},{'type':'qcode','data':SHIFTED.get(ch,ch.lower())}],'hold-time':30});time.sleep(.07)
        else:q.key(PLAIN.get(ch,ch))
def command(text,seconds=180,wait_exit=True):
    print('GUEST:',text,flush=True);offset=len(log());type_text(text);q.key('ret')
    if wait_exit:wait(lambda:'Application exited:' in log()[offset:],seconds)
    else:time.sleep(.5)
    return log()[offset:]
def memory(address,size):
    path=(folder/'memory.bin').resolve();q.call('pmemsave',{'val':address,'size':size,'filename':str(path)});return path.read_bytes()
def symbols(path):
    return {line.split()[2]:int(line.split()[0],16) for line in subprocess.check_output([args.nm,'-n',str(path)],text=True).splitlines() if len(line.split())==3}
desktop=symbols(folder/'desktop.elf');kernel=symbols(folder/'kernel.elf')
def screen_lines():
    raw=memory(desktop['lines']-0x400000+0x2000000,17*82);return '\n'.join(raw[i*82:(i+1)*82].split(b'\0')[0].decode(errors='replace') for i in range(17))
def counter(name):return struct.unpack('<Q',memory(kernel[name],8))[0]
def check(label,condition):
    assert condition,label+'\n'+log()[-6000:]
    results.append(label);print('PASS:',label,flush=True)
def gpt_state():
    """(primary_ok, backup_ok) judged on the host by signature and both CRCs."""
    data=disk.read_bytes();size=len(data)
    def ok(lba):
        header=bytearray(data[lba*512:lba*512+512])
        if header[:8]!=b'EFI PART':return False
        length,checksum=struct.unpack_from('<II',header,12);struct.pack_into('<I',header,16,0)
        if zlib.crc32(header[:length])!=checksum or struct.unpack_from('<Q',header,24)[0]!=lba:return False
        table,count,entry,crc=struct.unpack_from('<QIII',header,72);return zlib.crc32(data[table*512:table*512+count*entry])==crc
    return ok(1),ok(size//512-1)
def open_disk():
    """The image lives under a synced folder; retry while a background indexer holds it."""
    for attempt in range(50):
        try:return disk.open('r+b')
        except PermissionError:
            if attempt==49:raise
            time.sleep(.2)
def damage(lba,offset,count=8):
    with open_disk() as f:f.seek(lba*512+offset);original=f.read(count);f.seek(lba*512+offset);f.write(bytes(b^0xff for b in original))
def expected_hash():
    h=hashlib.sha256()
    for i in range(256):
        seed=i+1;out=bytearray(4096)
        for n in range(4096):seed=(seed*1103515245+12345)&0xffffffff;out[n]=(seed>>16)&0xff
        h.update(out)
    return h.hexdigest()
try:
    # 1. Fresh copy: build tools, check, semantics, unified namespace.
    boot()
    check('GPT partitions discovered with both copies intact','GPT: development and exchange partitions discovered' in log() and 'damaged' not in log())
    check('Fresh copy mounts as cleanly unmounted','did not unmount cleanly' not in log())
    out=command('gcc -O2 fsck-aurora.c -o fsck-aurora');check('Guest builds fsck-aurora','Application exited: 0' in out)
    out=command('gcc -O2 filesystem.c -o filesystem');check('Guest builds filesystem regression','Application exited: 0' in out)
    out=command('gcc -O2 interrupted-writes.c -o interrupted-writes');check('Guest builds interrupted-writes workload','Application exited: 0' in out)
    command('sync');out=command('./fsck-aurora -v',seconds=600);print(out)
    check('fsck-aurora reports the fresh volumes clean','FSCK: clean' in out and 'Application exited: 0' in out)
    out=command('./filesystem',seconds=300);print(out)
    for label in ['hard links share one inode','symlinks resolve, lstat stops at the link','ownership, modes and timestamps persist','directory link counts follow subdirectories','unlink and rename keep open descriptors, orphans vanish on close','AuroraFS is reachable as /aurorafs','FAT exchange volume stamps time and parks open files','raw devices are readable and read-only']:
        check(label,'PASS '+label in out)
    check('Filesystem regression exit status','FILESYSTEM REGRESSION OK' in out and 'Application exited: 0' in out)
    command('save',wait_exit=False);check('Desktop saves notes through the legacy API','Notes saved' in screen_lines())
    out=command('bash fs-bridge.sh');print(out)
    check('Native process reads the legacy-written notes.txt through /aurorafs','Welcome to your own little operating system.' in out and 'BRIDGE_DONE' in out)
    # The desktop console keeps 17 lines; AuroraFS names scroll off once the /work listing follows them.
    command('ls',wait_exit=False);lines=screen_lines();check('Desktop ls walks AuroraFS and then the development volume without error','error' not in lines.lower() and any(name in lines for name in ('fsck-aurora','filesystem','interrupted-writes','hellowork','fs-bridge.sh')))
    command('cat bridge.txt',wait_exit=False);check('Desktop reads the native-written AuroraFS file','native-side' in screen_lines())
    command('load',wait_exit=False);check('Legacy read falls back to /work when AuroraFS lacks the name','Notes loaded from notes.txt.' in screen_lines())
    command('save fs-check.txt',wait_exit=False);check('Desktop writes loaded notes to the development volume','Source saved' in screen_lines())
    out=command('hellowork aurora');check('SDK application spawns from /work on the development volume','Hello, aurora!' in screen_lines() and 'Application exited: 0' in out)
    out=command('sync');check('sync marks the volume clean','Application exited: 0' in out)
    stop()
    # 2. A clean stop leaves a clean superblock; a power cut mid-write does not.
    boot()
    check('Reboot after sync sees a clean superblock','did not unmount cleanly' not in log() and 'reclaimed crash orphans' not in log())
    offset=len(log());command('./interrupted-writes',wait_exit=False)
    wait(lambda:'IW_FAT_ORPHAN' in log()[offset:] and 'IW_PROGRESS 12' in log()[offset:],300)
    stop(hard=True);check('Power cut while the workload streams writes',True)
    # 3. Recovery: unclean mark, orphans reclaimed, fsync'd data intact, no structural damage.
    boot()
    check('Kernel detects the unclean stop','EXT2: previous session did not unmount cleanly' in log())
    check('Both parked crash orphans are reclaimed at mount','VFS: reclaimed crash orphans count=0000000000000002' in log())
    check('Recovery counters agree',counter('vfs_orphans_reclaimed')==2 and counter('ext2_unclean_mounts')==1)
    out=command('bash fs-verify.sh');print(out)
    check('Data committed with fsync before the power cut hashes correctly',expected_hash() in out and 'work-notes' in out)
    lines=[l for l in out.splitlines() if l.strip().isdigit()]
    check('No orphan names remain in either volume root',lines and lines[0].strip()=='0')
    command('sync');out=command('./fsck-aurora',seconds=600);print(out)
    check('fsck-aurora finds no structural damage after the power cut','FSCK:' in out and 'ERROR:' not in out and ('Application exited: 0' in out or 'Application exited: 1' in out))
    out=command('./filesystem',seconds=300);check('Filesystem regression passes after recovery','FILESYSTEM REGRESSION OK' in out)
    command('sync');stop()
    # 4. GPT copies: damage the primary, then the backup, then both.
    check('Both GPT copies valid before damage',gpt_state()==(True,True))
    damage(1,0);check('Primary GPT header damaged on the host',gpt_state()==(False,True))
    boot();check('Kernel recovers partitions from the backup GPT','GPT: primary header damaged; recovered from backup' in log() and 'GPT: primary header rewritten from backup' in log() and 'EXT2: writable development volume mounted' in log())
    check('GPT counters record the recovery',counter('gpt_backup_recoveries')==1 and counter('gpt_repairs')==1)
    out=command('./fsck-aurora --no-boot',seconds=600);check('fsck-aurora sees both copies valid after repair','primary valid, backup valid' in out)
    command('sync');stop();check('Primary GPT restored on disk',gpt_state()==(True,True))
    size=disk.stat().st_size//512;damage(size-33,64);check('Backup GPT table damaged on the host',gpt_state()==(True,False))
    boot();check('Kernel rewrites the damaged backup from the primary','GPT: backup header damaged; rewritten from primary' in log() and 'EXT2: writable development volume mounted' in log())
    command('sync');stop();check('Backup GPT restored on disk',gpt_state()==(True,True))
    saved=disk.read_bytes()[512:1024];damage(1,16,4);damage(size-1,16,4)
    boot();check('Both copies damaged is reported and the boot disk still serves the desktop','GPT: both headers damaged' in log() and 'desktop ready' in log())
    stop()
    with open_disk() as f:f.seek(512);f.write(saved)
    check('Backup still damaged after the host restores only the primary',gpt_state()==(True,False))
    boot();check('Primary repairs the backup again after host restore','GPT: backup header damaged; rewritten from primary' in log());command('sync');stop()
    check('Both GPT copies valid at the end',gpt_state()==(True,True))
    # 5. ATA attachment: IDENTIFY sizes /dev/disk for the checker.
    ata=True;boot();check('ATA attachment mounts the same volumes','EXT2: writable development volume mounted' in log())
    out=command('./fsck-aurora --no-boot',seconds=900);check('fsck-aurora runs over the ATA raw device','Checking /dev/disk' in out and 'sectors)' in out and '(0 sectors)' not in out and 'ERROR:' not in out)
    command('sync');stop()
    print(f'\nALL {len(results)} FILESYSTEM CHECKS PASSED')
finally:
    stop()

