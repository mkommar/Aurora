"""Check the development image with independent Linux filesystem checkers.
The VM receives the disk read-only; e2fsck and fsck.fat run in no-write mode.
"""
import argparse,functools,http.server,importlib.util,subprocess,threading
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('image',nargs='?',default='build/development.img');args=p.parse_args()
image=Path(args.image).resolve()
if not image.is_file():raise SystemExit('Image does not exist')
spec=importlib.util.spec_from_file_location('builder','build-gnu-bootstrap.py');builder=importlib.util.module_from_spec(spec);spec.loader.exec_module(builder)
root=builder.ROOT;builder.initrd()
script='''#!/bin/sh
set -eu
apk add --no-cache e2fsprogs dosfstools
modprobe virtio_blk
e2fsck -f -n /dev/vda1
fsck.fat -n /dev/vda2
echo AURORA_FILESYSTEM_CHECKS_PASSED
'''
(root/'bootstrap-gnu.sh').write_bytes(script.encode())
server=http.server.ThreadingHTTPServer(('127.0.0.1',8879),functools.partial(http.server.SimpleHTTPRequestHandler,directory=str(root)))
threading.Thread(target=server.serve_forever,daemon=True).start()
logfile=Path('build/filesystem-check.log').resolve()
command=['tools/qemu/qemu-system-x86_64.exe','-machine','pc','-accel','whpx','-cpu','qemu64','-m','512M',
    '-kernel',str(root/'vmlinuz-virt'),'-initrd',str(root/'builder-initramfs.gz'),'-append','console=ttyS0 rdinit=/init panic=1',
    '-netdev','user,id=net0','-device','virtio-net-pci,netdev=net0','-display','none','-serial',f'file:{logfile}',
    '-drive',f'format=raw,file={image},if=none,id=check,readonly=on','-device','virtio-blk-pci,drive=check','-no-reboot']
try:
    process=subprocess.Popen(command,creationflags=subprocess.CREATE_NO_WINDOW)
    try:code=process.wait(timeout=300)
    except BaseException:process.terminate();process.wait();raise
    output=logfile.read_text(errors='replace');print(output[-6000:])
    if code or 'AURORA_FILESYSTEM_CHECKS_PASSED' not in output or 'Fix? no' in output:raise SystemExit('Filesystem validation failed')
finally:server.shutdown();server.server_close()
