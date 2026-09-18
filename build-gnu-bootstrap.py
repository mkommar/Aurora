"""Build pinned GNU sources in an isolated QEMU Linux VM (1 GiB, no host install)."""
from pathlib import Path
import gzip,tarfile,stat,subprocess,threading,http.server,functools,time,base64
ROOT=Path('tools/gnu-bootstrap').resolve()
def initrd():
    # Kernel and modules come from the same pinned package.
    entries={'dev':(stat.S_IFDIR|0o755,b'',0,0),'dev/console':(stat.S_IFCHR|0o600,b'',5,1)}
    with tarfile.open(ROOT/'linux-virt-6.18.52-r0.apk',ignore_zeros=True) as archive:
        for m in archive:
            name=m.name.removeprefix('./').rstrip('/')
            if name=='boot/vmlinuz-virt':(ROOT/'vmlinuz-virt').write_bytes(archive.extractfile(m).read())
            if not name.startswith('lib/modules/'):continue
            if m.isfile():entries[name]=(stat.S_IFREG|m.mode,archive.extractfile(m).read(),0,0)
            elif m.isdir():entries[name]=(stat.S_IFDIR|m.mode,b'',0,0)
    with tarfile.open(ROOT/'alpine-minirootfs-3.23.0-x86_64.tar.gz') as archive:
        for m in archive:
            name=m.name.removeprefix('./').rstrip('/')
            if not name:continue
            if m.isfile(): entries[name]=(stat.S_IFREG|m.mode,archive.extractfile(m).read(),0,0)
            elif m.isdir():entries[name]=(stat.S_IFDIR|m.mode,b'',0,0)
            elif m.issym():entries[name]=(stat.S_IFLNK|m.mode,m.linkname.encode(),0,0)
    script=b'''#!/bin/sh
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
exec </dev/console >/dev/console 2>&1
depmod -a
modprobe virtio_net
ip link set eth0 up
ip addr add 10.0.2.15/24 dev eth0
ip route add default via 10.0.2.2
echo nameserver 10.0.2.3 > /etc/resolv.conf
wget -O /build-script http://10.0.2.2:8879/bootstrap-gnu.sh
sh /build-script
echo AURORA_BOOTSTRAP_EXIT=$?
poweroff -f
'''
    entries['init']=(stat.S_IFREG|0o755,script,0,0)
    for name in list(entries):
        parts=name.split('/')
        for i in range(1,len(parts)):
            entries.setdefault('/'.join(parts[:i]),(stat.S_IFDIR|0o755,b'',0,0))
    out=bytearray()
    ordered=sorted(entries.items(),key=lambda item:(item[0].count('/'),item[0]))
    for inode,(name,(mode,data,major,minor)) in enumerate(ordered+[('TRAILER!!!',(0,b'',0,0))],1):
        name=name.encode()+b'\0'; fields=[inode,mode,0,0,1,0,len(data),0,0,major,minor,len(name),0]
        out.extend(b'070701'+''.join(f'{v:08x}' for v in fields).encode()+name);out.extend(b'\0'*(-len(out)%4))
        out.extend(data);out.extend(b'\0'*(-len(out)%4))
    (ROOT/'builder-initramfs.gz').write_bytes(gzip.compress(out,compresslevel=1))
class Handler(http.server.SimpleHTTPRequestHandler):
    def do_POST(self):
        if self.path!='/result':self.send_error(404);return
        length=int(self.headers.get('Content-Length','0'))
        if not 0<length<256*1024*1024:self.send_error(413);return
        with (ROOT/'gnu-bootstrap.tar.gz.part').open('wb') as out:
            remaining=length
            while remaining:
                chunk=self.rfile.read(min(remaining,1024*1024))
                if not chunk:raise OSError('Incomplete upload')
                out.write(chunk);remaining-=len(chunk)
        part=ROOT/'gnu-bootstrap.tar.gz.part'
        data=base64.b64decode(part.read_bytes())
        if not data.startswith(b'\x1f\x8b') or len(data)<1024:self.send_error(400);return
        part.write_bytes(data);part.replace(ROOT/'gnu-bootstrap.tar.gz')
        self.send_response(200);self.end_headers();self.wfile.write(b'OK')
if __name__=='__main__':
    initrd(); (ROOT/'bootstrap-gnu.sh').write_bytes(Path('bootstrap-gnu.sh').read_bytes())
    server=http.server.ThreadingHTTPServer(('127.0.0.1',8879),functools.partial(Handler,directory=str(ROOT)))
    threading.Thread(target=server.serve_forever,daemon=True).start()
    command=['tools/qemu/qemu-system-x86_64.exe','-machine','pc','-accel','whpx','-cpu','qemu64','-smp','2','-m','1G',
        '-kernel',str(ROOT/'vmlinuz-virt'),'-initrd',str(ROOT/'builder-initramfs.gz'),'-append','console=ttyS0 rdinit=/init panic=1',
        '-netdev','user,id=net0','-device','virtio-net-pci,netdev=net0','-display','none','-serial',f'file:{ROOT}/builder.log','-no-reboot']
    try:
        process=subprocess.Popen(command,creationflags=subprocess.CREATE_NO_WINDOW)
        try:code=process.wait(timeout=7200)
        except BaseException:process.terminate();process.wait();raise
        log=(ROOT/'builder.log').read_text(errors='replace')
        print(log[-8000:])
        if code or 'AURORA GNU BOOTSTRAP COMPLETE' not in log:raise SystemExit('GNU bootstrap did not complete; see tools/gnu-bootstrap/builder.log')
    finally:server.shutdown();server.server_close()
