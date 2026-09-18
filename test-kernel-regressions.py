"""Run the isolation and GUI suites on disposable, hidden 128 MiB VMs."""
import argparse, importlib.util, shutil, subprocess, sys, time
from pathlib import Path

parser=argparse.ArgumentParser();parser.add_argument('--nm',required=True);args=parser.parse_args()
spec=importlib.util.spec_from_file_location('qmp','tools-qmp.py');mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
for source,suite in [('build/selftest','test-microkernel.py'),('build','test-smoke.py')]:
    folder=Path('build/kernel-regressions')/Path(suite).stem;folder.mkdir(parents=True,exist_ok=True)
    for name in ['aurora.img','kernel.elf','desktop.elf']:shutil.copyfile(Path(source)/name,folder/name)
    with (folder/'qemu-stderr.log').open('w') as stderr:
        process=subprocess.Popen(['tools/qemu/qemu-system-x86_64.exe','-machine','pc','-accel','tcg','-cpu','qemu64','-m','128M',
            '-drive',f'format=raw,file={folder}/aurora.img','-display','none','-net','none','-serial',f'file:{folder}/serial.log',
            '-qmp','tcp:127.0.0.1:4444,server=on,wait=off'],creationflags=subprocess.CREATE_NO_WINDOW,stderr=stderr)
    q=None
    try:
        for _ in range(100):
            try:q=mod.QMP();break
            except OSError:time.sleep(.1)
        if q is None:raise RuntimeError((folder/'qemu-stderr.log').read_text())
        for _ in range(200):
            if (folder/'serial.log').exists() and 'desktop ready' in (folder/'serial.log').read_text():break
            time.sleep(.1)
        q.f.close();q.sock.close();q=None
        subprocess.run([sys.executable,suite,'--nm',args.nm,'--build-dir',str(folder)],check=True)
    finally:
        if q is None and process.poll() is None:
            try:q=mod.QMP()
            except OSError:pass
        if q:
            try:q.call('quit')
            except (OSError,ValueError):pass
        try:process.wait(timeout=10)
        except subprocess.TimeoutExpired:process.terminate();process.wait(timeout=10)
