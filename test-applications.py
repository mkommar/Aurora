"""End-to-end application/storage tests on an isolated copy of the normal disk.
Build first, then build tests/appcheck.c and tests/crash.c into build/tests.
Run with --nm PATH_TO_LLVM_NM. Uses QMP port 4445, no visible QEMU window.
"""
import argparse, importlib.util, json, shutil, struct, subprocess, time
from pathlib import Path

parser=argparse.ArgumentParser();parser.add_argument('--nm',required=True)
args=parser.parse_args()
spec=importlib.util.spec_from_file_location('qmp','tools-qmp.py')
mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
folder=Path('build/application-tests');folder.mkdir(exist_ok=True)
disk=folder/'aurora.img';shutil.copyfile('build/aurora.img',disk)
data=bytearray(disk.read_bytes())
def install(name,blob):
    for i in range(32):
        offset=513*512+i*64
        if not struct.unpack_from('<I',data,offset+36)[0]:
            data[offset:offset+64]=struct.pack('<32sII24x',name.encode(),len(blob),1)
            start=(520+i*128)*512;data[start:start+len(blob)]=blob;return
    raise AssertionError('Test image directory is full')
install('appcheck',Path('build/tests/appcheck.elf').read_bytes())
install('crash',Path('build/tests/crash.elf').read_bytes())
elf=Path('build/apps/hello.elf').read_bytes()
phoff=struct.unpack_from('<Q',elf,32)[0]
bad=bytearray(elf);struct.pack_into('<I',bad,phoff+4,7);install('badwx',bad)
bad=bytearray(elf);struct.pack_into('<Q',bad,phoff+8,0xfffffffffffff000);install('badoffset',bad)
bad=bytearray(elf);struct.pack_into('<Q',bad,24,0x5f0000);install('badentry',bad)
bad=bytearray(elf);struct.pack_into('<Q',bad,32,0xffffffffffffffff);install('badheader',bad)
bad=bytearray(elf);struct.pack_into('<Q',bad,phoff+56+16,0x400000);install('badoverlap',bad)
bad=bytearray(elf);struct.pack_into('<Q',bad,phoff+40,0xffffffffffffffff);install('badsize',bad)
install('truncated',elf[:20]);disk.write_bytes(data)

def symbols(path):
    result={}
    for line in subprocess.check_output([args.nm,'-n',str(path)],text=True).splitlines():
        fields=line.split()
        if len(fields)==3:result[fields[2]]=int(fields[0],16)
    return result
desktop=symbols('build/desktop.elf');kernel=symbols('build/kernel.elf')
results=[];q=None;process=None
def check(label,condition):
    assert condition,label
    results.append(label);print('PASS:',label,flush=True)
def log():return (folder/'serial.log').read_text(errors='replace')
def wait(predicate):
    deadline=time.monotonic()+8
    while time.monotonic()<deadline:
        if predicate():return
        time.sleep(.05)
    raise AssertionError('Timed out waiting for guest: '+log()[-2000:])
def boot():
    global process,q
    process=subprocess.Popen(['tools/qemu/qemu-system-x86_64.exe','-machine','pc','-accel','tcg','-cpu','qemu64',
        '-m','128M','-vga','std','-drive',f'format=raw,file={disk}','-serial',f'file:{folder}/serial.log',
        '-net','none','-qmp','tcp:127.0.0.1:4445,server=on,wait=off','-display','none'],creationflags=subprocess.CREATE_NO_WINDOW)
    deadline=time.monotonic()+8
    while True:
        try:q=mod.QMP(4445);break
        except OSError:
            if time.monotonic()>deadline:raise
            time.sleep(.1)
    wait(lambda:'desktop ready' in log());q.key('f2')
def shutdown():
    global q,process
    if q:
        try:q.call('quit')
        finally:q.f.close();q.sock.close();q=None
    if process:process.wait(timeout=10);process=None
def memory(address,size):
    path=(folder/'memory.bin').resolve();q.call('pmemsave',{'val':address,'size':size,'filename':str(path)});return path.read_bytes()
def screen_lines():
    raw=memory(desktop['lines']-0x400000+0x2000000,17*82)
    return '\n'.join(raw[i:i+82].split(b'\0')[0].decode(errors='replace') for i in range(0,len(raw),82))
def command(text,application=False):
    start=len(log())
    for c in text:q.key({' ':'spc','.':'dot','-':'minus'}.get(c,c))
    q.key('ret')
    if application:wait(lambda:'Application exited:' in log()[start:])
    else:time.sleep(.25)
    return log()[start:]
try:
    boot();check('Persistent filesystem mounts','FS: AuroraFS mounted read/write' in log())
    output=command('hello aurora',True);check('ELF application receives argv and prints to terminal','Hello, aurora!' in output and 'Hello, aurora!' in screen_lines())
    output=command('calc 12 30',True);check('C integer parsing and printf work','Result: 42' in output)
    output=command('filedemo',True);check('Application writes and reads a persistent file','Read back: Saved by a C application.' in output)
    command('cat message.txt');check('Shell can read files created by applications','Saved by a C application.' in screen_lines())
    output=command('appcheck alpha beta',True)
    check('Runtime, allocator, 64 KiB I/O, truncation, empty files and pointer validation pass','RUNTIME PASS' in output and 'RUNTIME FAIL' not in output)
    check('Exit status and integer formatting survive the ABI','Application exited: 7' in output and 'FORMAT -2147483648 42 ff A %' in output)
    root=struct.unpack('<Q',memory(kernel['task_cr3']+3*8,8))[0]
    code=struct.unpack('<Q',memory(root+0x6000,8))[0]
    guard=struct.unpack('<Q',memory(root+0x6000+495*8,8))[0]
    stack=struct.unpack('<Q',memory(root+0x6000+496*8,8))[0]
    check('Loaded application has RX code and a guarded RW/NX stack',code&7==5 and not(code>>63) and not(guard&1) and stack&7==7 and stack>>63==1)
    for name in ('badwx','badoffset','badentry','badheader','badoverlap','badsize','truncated','readme.txt'):
        command('clear');command('run '+name)
        check('Loader rejects '+name,'Invalid or unsupported ELF' in screen_lines())
    command('clear');command('missing');check('Missing executable is reported','File not found.' in screen_lines())
    output=command('crash',True);check('Application fault is contained and reported','FAULT isolated task=' in output and 'Application exited: -134' in output)
    output=command('hello',True);check('A faulted application slot can be reused','Hello from an isolated C application!' in output)
    q.key('f3');q.key('z');q.key('f2');command('save');check('Notes save through the filesystem API','Notes saved' in screen_lines())
    shutdown();boot()
    command('cat message.txt');check('Application file survives a full VM restart','Saved by a C application.' in screen_lines())
    command('load');notes=memory(desktop['notes']-0x400000+0x2000000,1600).split(b'\0')[0]
    check('Notes survive a full VM restart',notes.endswith(b'z'))
    output=command('hello again',True);check('Application loads after restart','Hello, again!' in output)
    check('No kernel or production service faults','KERNEL PANIC' not in log() and 'FAULT isolated task=' not in log())
    q.capture('applications-tested')
finally:
    shutdown()
(folder/'results.json').write_text(json.dumps({'passed':results},indent=2))
print(f'{len(results)} application/storage checks passed.')
