"""Build pinned curl/Mbed TLS using the existing pinned native GCC in a Linux VM."""
import importlib.util,hashlib,json,shutil,subprocess,threading,http.server,functools,base64
from pathlib import Path
spec=importlib.util.spec_from_file_location('builder','build-gnu-bootstrap.py');b=importlib.util.module_from_spec(spec);spec.loader.exec_module(b)
b.initrd();root=Path('tools/network-bootstrap').resolve();root.mkdir(parents=True,exist_ok=True)
for p in Path('tools/network-src').iterdir():
    if p.is_file():shutil.copyfile(p,root/p.name)
shutil.copyfile('tools/gcc-native/x86_64-linux-musl-native.tgz',root/'native.tgz')
shutil.copyfile('bootstrap-network.sh',root/'bootstrap-gnu.sh')
lock=json.loads(Path('network-sources.lock.json').read_text())
for name,v in lock.items():assert hashlib.sha256((root/name).read_bytes()).hexdigest()==v['sha256']
native=(root/'native.tgz').read_bytes()
assert hashlib.sha512(native).hexdigest()=='44d441ad9aa11a06feddf3daa4c9f53ad7d9ca37af1f5a61379aca07793703d179410cea723c1b7fca94c4de19a321228bdb3656bc5cbdb5e3bea8e2d6dac6c7'
(root/'sources.sha256').write_text('\n'.join(v['sha256']+'  '+n for n,v in lock.items() if not n.startswith('lwip'))+'\n'+hashlib.sha256(native).hexdigest()+'  native.tgz\n',encoding='utf-8',newline='\n')
class Handler(http.server.SimpleHTTPRequestHandler):
    def do_POST(self):
        if self.path!='/result':self.send_error(404);return
        n=int(self.headers.get('Content-Length','0'))
        if not 0<n<64*1024*1024:self.send_error(413);return
        raw=self.rfile.read(n);data=base64.b64decode(raw)
        if not data.startswith(b'\x1f\x8b'):self.send_error(400);return
        (root/'network-bootstrap.tar.gz').write_bytes(data);self.send_response(200);self.end_headers();self.wfile.write(b'OK')
server=http.server.ThreadingHTTPServer(('127.0.0.1',8879),functools.partial(Handler,directory=str(root)))
threading.Thread(target=server.serve_forever,daemon=True).start()
cmd=['tools/qemu/qemu-system-x86_64.exe','-machine','pc','-accel','whpx','-cpu','qemu64','-smp','2','-m','1536M',
    '-kernel',str(b.ROOT/'vmlinuz-virt'),'-initrd',str(b.ROOT/'builder-initramfs.gz'),'-append','console=ttyS0 rdinit=/init panic=1',
    '-netdev','user,id=net0','-device','virtio-net-pci,netdev=net0','-display','none','-serial',f'file:{root}/builder.log','-no-reboot']
try:
    process=subprocess.Popen(cmd,creationflags=subprocess.CREATE_NO_WINDOW)
    try:code=process.wait(timeout=5400)
    except BaseException:process.terminate();process.wait();raise
    text=(root/'builder.log').read_text(errors='replace');print(text[-4000:])
    assert code==0 and 'AURORA_NETWORK_BOOTSTRAP_COMPLETE' in text
finally:server.shutdown()
