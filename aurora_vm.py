"""Run a guest Bash script on a disposable Aurora disk, retaining serial logs.

The host only stages inputs and observes QMP; all commands execute in Aurora.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import struct
import time

from image_access import put_ext2_files
from qemu_iommu import write_dmar

spec = importlib.util.spec_from_file_location('qmp', 'tools-qmp.py')
qmp = importlib.util.module_from_spec(spec); spec.loader.exec_module(qmp)

def copy_disk(source,target):
    """Create a sparse disposable copy; never replace an existing destination."""
    with Path(source).open('rb') as incoming,Path(target).open('xb') as outgoing:
        if os.name=='nt':
            import ctypes as C
            import msvcrt
            returned=C.c_ulong()
            # FSCTL_SET_SPARSE applies only to this newly created output file.
            C.WinDLL('kernel32',use_last_error=True).DeviceIoControl(
                C.c_void_p(msvcrt.get_osfhandle(outgoing.fileno())),0x900c4,
                None,0,None,0,C.byref(returned),None)
        zeros=bytes(1024*1024);size=0
        while data:=incoming.read(len(zeros)):
            size+=len(data)
            if data==zeros or not any(data):outgoing.seek(len(data),1)
            else:outgoing.write(data)
        outgoing.truncate(size)

def counters(q,folder):
    default=r'C:/Program Files/Unity/Hub/Editor/6000.4.0f1/Editor/Data/PlaybackEngines/AndroidPlayer/NDK/toolchains/llvm/prebuilt/windows-x86_64/bin'
    llvm=os.environ.get('AURORA_LLVM')
    nm=Path(llvm)/('llvm-nm.exe' if os.name=='nt' else 'llvm-nm') if llvm else Path(shutil.which('llvm-nm') or (Path(default)/'llvm-nm.exe'))
    if not nm.exists(): return {}
    lines=subprocess.check_output([str(nm),'-n',str(folder/'kernel.elf')],text=True).splitlines()
    symbols={line.split()[2]:int(line.split()[0],16) for line in lines if len(line.split())==3}
    result={}
    for name in ('virtio_timeouts','virtio_late_completions','native_cache_evictions','native_lazy_commit_failures','native_fault_signals'):
        if name not in symbols: continue
        path=(folder/(name+'.bin')).resolve()
        q.call('pmemsave',{'val':symbols[name],'size':8,'filename':str(path)})
        result[name]=struct.unpack('<Q',path.read_bytes())[0]
    return result

def run(script, folder, disk='build/development.img', timeout=7200, cpus=4,
        port=4454, resume=False, files=None, network_ready=False, launch='bash aurora-job.sh',
        host_forward=None):
    folder = Path(folder); folder.mkdir(parents=True, exist_ok=True)
    target = folder/'development.img'
    if not resume:
        if target.exists(): raise ValueError('Output disk exists; use --resume or a new folder')
        copy_disk(disk, target)
    for name in ('aurora.img', 'kernel.elf', 'desktop.elf'):
        shutil.copyfile(Path('build')/name, folder/name)
    payload = dict(files or {})
    payload['/work/aurora-job.sh'] = Path(script).read_bytes()
    put_ext2_files(target, payload)
    write_dmar(folder/'qemu-dmar.bin')
    logpath = folder/'serial.log'
    qemu=os.environ.get('AURORA_QEMU') or shutil.which('qemu-system-x86_64') or 'tools/qemu/qemu-system-x86_64.exe'
    network = ['-netdev', 'user,id=net0']
    if host_forward:
        host_port, guest_port = host_forward
        network[1] += f',hostfwd=tcp:127.0.0.1:{host_port}-:{guest_port}'
    command = [qemu, '-machine', 'q35', '-accel', 'tcg,thread=multi',
        '-cpu', 'qemu64', '-smp', str(cpus), '-m', '1G', '-no-reboot', '-vga', 'std',
        '-drive', f'format=raw,file={folder}/aurora.img,if=ide,index=0',
        '-drive', f'format=raw,file={target},if=none,id=development',
        '-device', 'intel-iommu,intremap=on,dma-translation=on,aw-bits=48', '-acpitable', f'file={folder/"qemu-dmar.bin"}',
        '-device', 'virtio-blk-pci,drive=development,disable-legacy=on,iommu_platform=on',
        '-object', 'rng-builtin,id=rng0', '-device', 'virtio-rng-pci,rng=rng0,disable-modern=on',
        *network, '-device', 'virtio-net-pci,netdev=net0,disable-modern=on',
        '-serial', f'file:{logpath}', '-display', 'none',
        '-qmp', f'tcp:127.0.0.1:{port},server=on,wait=off']
    q = None
    outcome={'status':'running','script':str(script),'cpus':cpus}
    (folder/'results.json').write_text(json.dumps(outcome,indent=2),encoding='utf-8')
    with (folder/'qemu-stderr.log').open('w') as stderr:
        process = subprocess.Popen(command, stderr=stderr, creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    def log(): return logpath.read_text(errors='replace') if logpath.exists() else ''
    try:
        start = time.monotonic()
        while time.monotonic()-start < 60:
            if process.poll() is not None: raise RuntimeError((folder/'qemu-stderr.log').read_text())
            if q is None:
                try: q = qmp.QMP(port)
                except OSError: pass
            state=log()
            if q and 'desktop ready' in state and (not network_ready or 'NET: DHCP' in state): break
            time.sleep(.2)
        else: raise TimeoutError('Boot timeout: '+log()[-4000:])
        q.key('f2')
        for ch in launch: q.key({' ':'spc', '.':'dot', '-':'minus','/':'slash'}.get(ch, ch))
        offset=len(log()); q.key('ret'); printed=offset
        while time.monotonic()-start < timeout:
            data=log()
            if len(data)>printed:
                for line in data[printed:].splitlines():
                    if not line.startswith('NATIVE EXEC:'): print(line, flush=True)
                printed=len(data)
            tail=data[offset:]
            if 'KERNEL PANIC:' in tail:
                outcome['counters']=counters(q,folder)
                registers=q.call('human-monitor-command', {'command-line':'info registers'})
                (folder/'panic-registers.txt').write_text(registers)
                raise RuntimeError('Guest kernel panic: '+tail[-4000:])
            if 'Application exited:' in tail:
                outcome['counters']=counters(q,folder)
                if 'Application exited: 0' not in tail:
                    q.call('pmemsave',{'val':0,'size':0x70000,'filename':str((folder/'failure-kernel.bin').resolve())})
                    q.call('pmemsave',{'val':0x04000000,'size':0x80000,'filename':str((folder/'failure-state.bin').resolve())})
                    q.call('pmemsave',{'val':0x0d000000,'size':0x100000,'filename':str((folder/'failure-virtio.bin').resolve())})
                    raise RuntimeError('Guest script failed: '+tail[-8000:])
                outcome['status']='passed'
                return target
            if process.poll() is not None: raise RuntimeError('Guest exited unexpectedly: '+tail[-4000:])
            time.sleep(1)
        raise TimeoutError('Guest build timeout: '+log()[-8000:])
    except BaseException as error:
        outcome.update(status='failed',error=str(error))
        raise
    finally:
        (folder/'results.json').write_text(json.dumps(outcome,indent=2),encoding='utf-8')
        if q:
            try: q.call('quit')
            except (OSError, ValueError): pass
            try: q.f.close()
            except OSError: pass
            q.sock.close()
        if process.poll() is None:
            try: process.wait(timeout=15)
            except subprocess.TimeoutExpired: process.terminate(); process.wait(timeout=15)

if __name__ == '__main__':
    p=argparse.ArgumentParser(); p.add_argument('script'); p.add_argument('--folder', required=True)
    p.add_argument('--disk', default='build/development.img'); p.add_argument('--resume', action='store_true')
    p.add_argument('--timeout', type=int, default=7200); p.add_argument('--cpus', type=int, default=4)
    p.add_argument('--port', type=int, default=4454)
    a=p.parse_args(); print(run(a.script,a.folder,a.disk,a.timeout,a.cpus,a.port,a.resume))
