"""Run an Aurora-hosted apt-style HTTP repository and a separate Aurora client VM."""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import shutil
import socket
import subprocess
import tarfile
import tempfile
import os
import time

from image_access import put_ext2_files

spec = importlib.util.spec_from_file_location('qmp', 'tools-qmp.py')
qmp = importlib.util.module_from_spec(spec); spec.loader.exec_module(qmp)
parser = argparse.ArgumentParser()
parser.add_argument('--disk', default='build/development-thread.img')
parser.add_argument('--folder', default='build/apt-repository-pair')
parser.add_argument('--server-port', type=int, default=8080)
parser.add_argument('--bash-binary', default=os.environ.get('AURORA_BASH_BINARY'),
                    help='Bash executable compatible with Aurora musl to stage in the client VM')
args = parser.parse_args()


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def write_tar(path, entries):
    with tarfile.open(path, 'w:gz') as archive:
        for name, data in sorted(entries.items()):
            info = tarfile.TarInfo(name)
            info.size = len(data); info.mtime = 0; info.uid = info.gid = 0
            info.uname = info.gname = ''; info.mode = 0o644
            import io
            archive.addfile(info, io.BytesIO(data))


def repository(folder):
    control = (b'Package: aurora-apt-probe\nVersion: 1.0-1\nArchitecture: musl-linux-amd64\n'
               b'Maintainer: Aurora Test\nDescription: QEMU guest repository connectivity probe\n')
    deb = folder / 'aurora-apt-probe.deb'
    control_tar = folder / 'control.tar.gz'; data_tar = folder / 'data.tar.gz'
    write_tar(control_tar, {'./control': control})
    write_tar(data_tar, {'./opt/aurora/share/apt-probe/installed.txt': b'guest repository payload\n'})
    (folder / 'debian-binary').write_bytes(b'2.0\n')
    subprocess.run(['ar', 'crD', str(deb), 'debian-binary', control_tar.name, data_tar.name], cwd=folder, check=True)
    deb_name = 'aurora-apt-probe_1.0-1_musl-linux-amd64.deb'
    deb_bytes = deb.read_bytes(); deb_hash = hashlib.sha256(deb_bytes).hexdigest()
    package_path = f'pool/{deb_name}'
    packages = (f'Package: aurora-apt-probe\nVersion: 1.0-1\nArchitecture: musl-linux-amd64\n'
                f'Filename: {package_path}\nSize: {len(deb_bytes)}\nSHA256: {deb_hash}\n'
                'Description: QEMU guest repository connectivity probe\n\n').encode()
    package_hash = hashlib.sha256(packages).hexdigest()
    release = (f'Origin: Aurora QEMU test\nLabel: Aurora test repository\nSuite: stable\n'
               f'Codename: stable\nArchitectures: musl-linux-amd64\nComponents: main\nSHA256:\n'
               f' {package_hash} {len(packages)} dists/stable/main/binary-musl/Packages\n').encode()
    files = {
        '/work/apt-repo-server': Path('build/apt-repo-server').read_bytes(),
        '/work/apt-repo/dists/stable/main/binary-musl/Packages': packages,
        '/work/apt-repo/dists/stable/Release': release,
        '/work/apt-repo/' + package_path: deb_bytes,
    }
    return files


class VM:
    def __init__(self, image, port, host_forward=None):
        self.port = port; self.q = None; self.logpath = image.parent / 'serial.log'
        stderr = image.parent / 'qemu-stderr.log'
        qemu = shutil.which('qemu-system-x86_64')
        if not qemu: raise RuntimeError('qemu-system-x86_64 is not on PATH')
        network = ['-netdev', 'user,id=net0' + (f',hostfwd=tcp:127.0.0.1:{host_forward}-:{args.server_port}' if host_forward else ''),
                   '-device', 'virtio-net-pci,netdev=net0,disable-modern=on']
        cmd = [qemu,'-machine','pc','-accel','tcg','-cpu','qemu64','-smp','1','-m','512M','-no-reboot','-vga','std',
               '-drive',f'format=raw,file={(image.parent/"aurora.img").resolve()},if=ide,index=0',
               '-drive',f'format=raw,file={image.resolve()},if=none,id=development','-device','virtio-blk-pci,drive=development,disable-modern=on',
               '-object','rng-builtin,id=rng0','-device','virtio-rng-pci,rng=rng0,disable-modern=on',*network,
               '-serial',f'file:{self.logpath}','-display','none','-qmp',f'tcp:127.0.0.1:{port},server=on,wait=off']
        with stderr.open('w') as error_log:
            self.process = subprocess.Popen(cmd, stderr=error_log)
        self.stderr = stderr
        deadline=time.monotonic()+60
        while time.monotonic()<deadline:
            if self.process.poll() is not None: raise RuntimeError(stderr.read_text(errors='replace'))
            try: self.q=qmp.QMP(port); break
            except OSError: time.sleep(.2)
        if not self.q: raise TimeoutError(f'QMP did not start on port {port}: {stderr.read_text(errors="replace")}')
        self.wait('AURORA: desktop ready',60)
        self.q.key('f2')

    def log(self):
        return self.logpath.read_text(errors='replace') if self.logpath.exists() else ''

    def wait(self, marker, seconds=60):
        deadline=time.monotonic()+seconds
        while time.monotonic()<deadline:
            if marker in self.log(): return
            if self.process.poll() is not None: raise RuntimeError(self.stderr.read_text(errors='replace')+'\n'+self.log())
            time.sleep(.1)
        raise TimeoutError(f'Waiting for {marker!r}:\n'+self.log()[-5000:])

    def command(self, text, marker, seconds=180):
        start=len(self.log())
        for char in text:
            if char==':':
                self.q.call('send-key',{'keys':[{'type':'qcode','data':'shift'},{'type':'qcode','data':'semicolon'}],'hold-time':30});time.sleep(.06)
            elif char.isupper() or char=='_':
                key='minus' if char=='_' else char.lower()
                self.q.call('send-key',{'keys':[{'type':'qcode','data':'shift'},{'type':'qcode','data':key}],'hold-time':30});time.sleep(.06)
            else:self.q.key({' ':'spc','-':'minus','.':'dot','/':'slash'}.get(char,char))
        self.q.key('ret')
        deadline=time.monotonic()+seconds
        while time.monotonic()<deadline:
            current=self.log()
            if marker in current[start:]:return current
            if self.process.poll() is not None:raise RuntimeError(self.stderr.read_text(errors='replace')+'\n'+current)
            time.sleep(.1)
        raise TimeoutError(f'Waiting for new {marker!r}:\n'+self.log()[-5000:])

    def close(self):
        if self.q:
            try:self.q.call('quit')
            except (OSError,ValueError):pass
            try:self.q.f.close()
            except OSError:pass
            self.q.sock.close();self.q=None
        if self.process.poll() is None:self.process.wait(timeout=15)


def main():
    root=Path(args.folder);root.mkdir(parents=True,exist_ok=True)
    host_port=free_port();server_qmp=free_port();client_qmp=free_port()
    server_dir=root/'server';client_dir=root/'client'
    server_dir.mkdir(exist_ok=True);client_dir.mkdir(exist_ok=True)
    server_disk=server_dir/'development.img';client_disk=client_dir/'development.img'
    shutil.copyfile('build/aurora.img',server_dir/'aurora.img')
    shutil.copyfile('build/aurora.img',client_dir/'aurora.img')
    shutil.copyfile(args.disk,server_disk);shutil.copyfile(args.disk,client_disk)
    musl='musl-gcc'
    subprocess.run([musl,'-static','-O2','-Wall','-Wextra','-Werror','tests/apt-repo-server.c','-o','build/apt-repo-server'],check=True)
    client=Path('build/apt-repo-client')
    subprocess.run([musl,'-static','-O2','-Wall','-Wextra','-Werror','tests/apt-repo-client.c','-o',str(client)],check=True)
    from image_access import put_ext2_files
    with tempfile.TemporaryDirectory(prefix='aurora-apt-fixture-') as temp:
        repo_payload=repository(Path(temp));repo_payload['/work/apt-repo-client']=client.read_bytes()
        put_ext2_files(server_disk,{'/work/apt-repo-server':Path('build/apt-repo-server').read_bytes(),**{k:v for k,v in repo_payload.items() if k.startswith('/work/apt-repo/')}})
        put_ext2_files(client_disk,{'/work/apt-repo-client':client.read_bytes()})
    server=client_vm=None
    tls_servers=[]
    try:
        if args.bash_binary and not Path(args.bash_binary).is_file():raise FileNotFoundError(args.bash_binary)
        from network_artifacts import files as network_files
        from network_test_fixture import start as start_tls_fixture
        tls_dir=root/'tls-fixture';tls_dir.mkdir(exist_ok=True);tls_servers,fixture_files=start_tls_fixture(tls_dir)
        tools=network_files()
        payload={path:data for path,data in tools.items() if path in ('/bin/curl','/etc/ssl/cert.pem','/etc/resolv.conf',
                                                                        '/lib/ld-musl-x86_64.so.1','/lib/libc.so')}
        payload['/work/test-ca.pem']=fixture_files['/work/test-ca.pem']
        if args.bash_binary:
            payload['/bin/bash']=Path(args.bash_binary).read_bytes();payload['/usr/bin/bash']=Path(args.bash_binary).read_bytes()
            payload['/work/apt-repo-curl-check.sh']=Path('tests/apt-repo-curl-check.sh').read_bytes()
        put_ext2_files(client_disk,payload)
        expected_downloads={
            '/work/curl-Packages':repo_payload['/work/apt-repo/dists/stable/main/binary-musl/Packages'],
            '/work/curl-Release':repo_payload['/work/apt-repo/dists/stable/Release'],
            '/work/curl-probe.deb':repo_payload['/work/apt-repo/pool/aurora-apt-probe_1.0-1_musl-linux-amd64.deb'],
            '/t':bytes(range(256))*1024,
        }
        if args.bash_binary:
            expected_downloads['/work/curl-tls.c']=b'#include <stdio.h>\nint main(void){puts("DOWNLOADED_TLS_SOURCE_COMPILED_IN_AURORA");return 0;}\n'
        server=VM(server_disk,server_qmp,host_port);server.wait('NET: DHCP',60)
        server.command(f'./apt-repo-server {9 if args.bash_binary else 6}','APT_REPO_SERVER_READY',60)
        client_vm=VM(client_disk,client_qmp);client_vm.wait('NET: DHCP',60)
        output=client_vm.command(f'./apt-repo-client {host_port}','APT_REPO_CLIENT_PASS',90)
        client_vm.wait('Application exited: 0',20);output=client_vm.log()
        if 'Application exited: 0' not in output: raise AssertionError(output[-5000:])
        out=client_vm.command('curl --version','Application exited: 0',60)
        if 'mbedTLS/3.6.7' not in out:raise AssertionError('curl TLS backend unavailable:\n'+out[-3000:])
        for remote,local in (('/P','/work/curl-Packages'),('/R','/work/curl-Release'),('/D','/work/curl-probe.deb')):
            curl_command=f'curl -fsS http://10.0.2.2:{host_port}{remote} -o {local}'
            out=client_vm.command(curl_command,'Application exited: 0',60)
            if 'Application exited: 0' not in out:raise AssertionError(out[-3000:])
        tls_command='curl -sS --cacert /work/test-ca.pem https://10.0.2.2:8881/s -o /t'
        if len(tls_command)>77:raise ValueError('TLS probe command exceeds Aurora desktop command length')
        out=client_vm.command(tls_command,'Application exited: 0',60)
        if 'Application exited: 0' not in out:raise AssertionError(out[-3000:])
        print('PASS client guest curl uses Mbed TLS and validates HTTPS through QEMU NAT')
        if args.bash_binary:
            out=client_vm.command('bash --version','Application exited: 0',60)
            if 'GNU bash, version' not in out:raise AssertionError('Bash startup failed:\n'+out[-3000:])
            print('PASS client guest Bash starts and reports its version')
            out=client_vm.command(f'bash /work/apt-repo-curl-check.sh {host_port}','Application exited: 0',120)
            if 'APT_REPO_BASH_CURL_TLS_PASS' not in out:raise AssertionError('Bash/curl repository check failed:\n'+out[-5000:])
            print('PASS guest Bash ran curl repository and verified-TLS checks')
        server.wait('APT_REPO_SERVED /pool/aurora-apt-probe_1.0-1_musl-linux-amd64.deb',30)
        server.wait('APT_REPO_SERVER_DONE',30)
        client_vm.close();client_vm=None
        from image_access import read_ext2_files
        downloaded=read_ext2_files(client_disk,expected_downloads)
        for path,expected in expected_downloads.items():
            if downloaded[path]!=expected:raise AssertionError(f'curl content mismatch: {path}')
        print('PASS curl fetched and verified repository metadata, package archive, and HTTPS payload')
        print('PASS two Aurora guests exchanged apt Packages, Release and .deb through QEMU NAT host forwarding')
        print(f'QEMU forward: client 10.0.2.2:{host_port} -> server guest :{args.server_port}')
    finally:
        if client_vm: client_vm.close()
        if server: server.close()
        for tls_server in tls_servers:tls_server.shutdown();tls_server.server_close()


if __name__=='__main__': main()
