"""Check CPU protection, IPC and scheduling against the deliberate-fault image.
Start run.ps1 -SelfTest -Headless, then pass --nm PATH_TO_LLVM_NM.
"""
import argparse, importlib.util, json, struct, subprocess, time
from pathlib import Path
spec=importlib.util.spec_from_file_location('qmp','tools-qmp.py')
mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
p=argparse.ArgumentParser();p.add_argument('--nm',required=True)
p.add_argument('--build-dir',default='build/selftest');a=p.parse_args()
build=Path(a.build_dir)
symbols={}
for line in subprocess.check_output([a.nm,'-n',str(build/'kernel.elf')],text=True).splitlines():
    f=line.split()
    if len(f)==3:symbols[f[2]]=int(f[0],16)
q=mod.QMP();results=[]
def check(label,ok):
    assert ok,label
    results.append(label);print('PASS:',label)
def memory(addr,size):
    path=(build/'memory-probe.bin').resolve()
    q.call('pmemsave',{'val':addr,'size':size,'filename':str(path)})
    return path.read_bytes()
def words(addr,count=1):return struct.unpack('<'+'Q'*count,memory(addr,8*count))
def counter(name):return words(symbols[name])[0]
def leaf(root,va):
    pd=words(root+0x2000+(va//0x200000)*8)[0]
    if pd&128:return pd
    return words((pd&0x000ffffffffff000)+((va//4096)%512)*8)[0]

# Wait for all probes and services, with a bounded startup deadline.
for _ in range(100):
    log=(build/'serial.log').read_text()
    if 'spinning without yielding' in log and log.count('FAULT isolated task=')==6 and 'desktop ready' in log:break
    time.sleep(.1)
check('All deliberate faults were contained',log.count('FAULT isolated task=')==6)
check('Invalid syscall buffers and capabilities rejected','syscall validation PASS' in log and 'FAILED' not in log)
check('Desktop started despite faulty peers','desktop ready' in log and 'KERNEL PANIC' not in log)

q.call('stop')
try:
    roots=words(symbols['task_cr3'],10)
    check('Every process has a distinct page-table root',len(set(roots))==10)
    faults=words(symbols['task_faults'],10)
    check('Kernel-memory and foreign-memory access produce page faults',faults[3]==15 and faults[4]==15)
    check('Direct port I/O produces a general-protection fault',faults[5]==14)
    check('Executing stack data produces a page fault',faults[7]==15)
    check('Writing executable code produces a page fault',faults[8]==15)
    check('Invalid opcode terminates only its process',faults[9]==7)
    check('Production services remain fault-free',faults[:3]==(0,0,0))
    for i,root in enumerate(roots[:3]):
        code=leaf(root,0x400000);data=leaf(root,0x5f0000)
        check(f'Process {i}: code is user RX; stack is user RW/NX',code&7==5 and not(code>>63) and data&7==7 and data>>63==1)
        check(f'Process {i}: kernel and foreign RAM are supervisor-only',not(leaf(root,0x10000)&4) and not(leaf(root,0x2000000)&4))
        check(f'Process {i}: stack guard is unmapped',not(leaf(root,0x5ef000)&1))
        check(f'Process {i}: bootstrap information is read-only',leaf(root,0x5d0000)&7==5)
    check('Shared surface: desktop writes, display reads, input has no access',
          leaf(roots[0],0x1000000)&7==7 and leaf(roots[2],0x1000000)&7==5 and not(leaf(roots[1],0x1000000)&4))
    fb=struct.unpack('<I',memory(0x928,4))[0]
    check('Only display has a user framebuffer mapping',leaf(roots[2],fb)&7==7 and all(not(leaf(roots[i],fb)&4) for i in [0,1]))
    check('Services have made IPC deliveries',counter('ipc_messages')>=3)
    before=counter('timer_ticks')
    runs=words(symbols['task_preemptions'],10)
    check('A non-yielding process was forcibly preempted',runs[6]>0)
finally:q.call('cont')
time.sleep(.2)
check('Timer and scheduler continue after faults',counter('timer_ticks')>before)
q.key('f2')
for key in ['h','e','l','p','ret']:q.key(key)
check('Input to desktop IPC still works after faults','COMMAND help' in (build/'serial.log').read_text())
q.capture('microkernel-isolation-tested')
(build/'microkernel-results.json').write_text(json.dumps({'passed':results},indent=2))
print(f'{len(results)} microkernel checks passed.')
