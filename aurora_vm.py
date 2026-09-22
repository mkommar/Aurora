"""Run a guest Bash script on a disposable Aurora disk, retaining serial logs.

The host only stages inputs and observes QMP; all commands execute in Aurora.
"""
import argparse
import importlib.util
from pathlib import Path
import shutil
import subprocess
import time

from image_access import put_ext2_files

spec = importlib.util.spec_from_file_location('qmp', 'tools-qmp.py')
qmp = importlib.util.module_from_spec(spec); spec.loader.exec_module(qmp)

def run(script, folder, disk='build/development.img', timeout=7200, cpus=4,
        port=4454, resume=False, files=None):
    folder = Path(folder); folder.mkdir(parents=True, exist_ok=True)
    target = folder/'development.img'
    if not resume:
        if target.exists(): raise ValueError('Output disk exists; use --resume or a new folder')
        shutil.copyfile(disk, target)
    for name in ('aurora.img', 'kernel.elf', 'desktop.elf'):
        shutil.copyfile(Path('build')/name, folder/name)
    payload = dict(files or {})
    payload['/work/aurora-job.sh'] = Path(script).read_bytes()
    put_ext2_files(target, payload)
    logpath = folder/'serial.log'
    command = ['tools/qemu/qemu-system-x86_64.exe', '-machine', 'pc', '-accel', 'tcg,thread=multi',
        '-cpu', 'qemu64', '-smp', str(cpus), '-m', '1G', '-no-reboot', '-vga', 'std',
        '-drive', f'format=raw,file={folder}/aurora.img,if=ide,index=0',
        '-drive', f'format=raw,file={target},if=none,id=development',
        '-device', 'virtio-blk-pci,drive=development,disable-modern=on',
        '-object', 'rng-builtin,id=rng0', '-device', 'virtio-rng-pci,rng=rng0,disable-modern=on',
        '-netdev', 'user,id=net0', '-device', 'virtio-net-pci,netdev=net0,disable-modern=on',
        '-serial', f'file:{logpath}', '-display', 'none',
        '-qmp', f'tcp:127.0.0.1:{port},server=on,wait=off']
    q = None
    with (folder/'qemu-stderr.log').open('w') as stderr:
        process = subprocess.Popen(command, stderr=stderr, creationflags=subprocess.CREATE_NO_WINDOW)
    def log(): return logpath.read_text(errors='replace') if logpath.exists() else ''
    try:
        start = time.monotonic()
        while time.monotonic()-start < 60:
            if process.poll() is not None: raise RuntimeError((folder/'qemu-stderr.log').read_text())
            if q is None:
                try: q = qmp.QMP(port)
                except OSError: pass
            if q and 'desktop ready' in log(): break
            time.sleep(.2)
        else: raise TimeoutError('Boot timeout: '+log()[-4000:])
        q.key('f2')
        for ch in 'bash aurora-job.sh': q.key({' ':'spc', '.':'dot', '-':'minus'}.get(ch, ch))
        offset=len(log()); q.key('ret'); printed=offset
        while time.monotonic()-start < timeout:
            data=log()
            if len(data)>printed:
                for line in data[printed:].splitlines():
                    if not line.startswith('NATIVE EXEC:'): print(line, flush=True)
                printed=len(data)
            tail=data[offset:]
            if 'KERNEL PANIC:' in tail:
                registers=q.call('human-monitor-command', {'command-line':'info registers'})
                (folder/'panic-registers.txt').write_text(registers)
                raise RuntimeError('Guest kernel panic: '+tail[-4000:])
            if 'Application exited:' in tail:
                if 'Application exited: 0' not in tail: raise RuntimeError('Guest script failed: '+tail[-8000:])
                return target
            if process.poll() is not None: raise RuntimeError('Guest exited unexpectedly: '+tail[-4000:])
            time.sleep(1)
        raise TimeoutError('Guest build timeout: '+log()[-8000:])
    finally:
        if q:
            try: q.call('quit')
            except (OSError, ValueError): pass
            q.f.close(); q.sock.close()
        if process.poll() is None:
            try: process.wait(timeout=15)
            except subprocess.TimeoutExpired: process.terminate(); process.wait(timeout=15)

if __name__ == '__main__':
    p=argparse.ArgumentParser(); p.add_argument('script'); p.add_argument('--folder', required=True)
    p.add_argument('--disk', default='build/development.img'); p.add_argument('--resume', action='store_true')
    p.add_argument('--timeout', type=int, default=7200); p.add_argument('--cpus', type=int, default=4)
    p.add_argument('--port', type=int, default=4454)
    a=p.parse_args(); print(run(a.script,a.folder,a.disk,a.timeout,a.cpus,a.port,a.resume))
