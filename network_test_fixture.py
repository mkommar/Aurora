"""Loopback-only HTTP/TLS/TCP/UDP fixtures reached through QEMU user NAT."""
import datetime, hashlib, http.server, ipaddress, socketserver, ssl, threading
from pathlib import Path
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID

def start(folder):
    now=datetime.datetime.now(datetime.timezone.utc)
    key=rsa.generate_private_key(public_exponent=65537,key_size=2048)
    name=x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,'Aurora isolated test CA')])
    ca=(x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
        .serial_number(x509.random_serial_number()).not_valid_before(now-datetime.timedelta(days=2))
        .not_valid_after(now+datetime.timedelta(days=2)).add_extension(x509.BasicConstraints(ca=True,path_length=0),True)
        .sign(key,hashes.SHA256()))
    pem=serialization.Encoding.PEM
    (folder/'test-key.pem').write_bytes(key.private_bytes(pem,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
    for label,expired in [('valid',False),('expired',True)]:
        cert=(x509.CertificateBuilder().subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,'Aurora fixture')]))
            .issuer_name(name).public_key(key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now-datetime.timedelta(days=2)).not_valid_after(now+datetime.timedelta(days=-1 if expired else 1))
            .add_extension(x509.SubjectAlternativeName([x509.IPAddress(ipaddress.ip_address('10.0.2.2'))]),False)
            .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]),False).sign(key,hashes.SHA256()))
        (folder/(label+'.pem')).write_bytes(cert.public_bytes(pem))
    source=b'#include <stdio.h>\nint main(void){puts("DOWNLOADED_TLS_SOURCE_COMPILED_IN_AURORA");return 0;}\n'
    payload=bytes(range(256))*1024
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version='HTTP/1.1'
        def log_message(self,*args):pass
        def do_GET(self):
            if self.path=='/redirect':
                self.send_response(302);self.send_header('Location','/payload');self.send_header('Content-Length','0');self.end_headers();return
            data=source if self.path=='/source.c' else payload
            self.send_response(200)
            if self.path=='/chunked':
                self.send_header('Transfer-Encoding','chunked');self.end_headers()
                for i in range(0,len(data),997):
                    piece=data[i:i+997];self.wfile.write(('%x\r\n'%len(piece)).encode()+piece+b'\r\n')
                self.wfile.write(b'0\r\n\r\n')
            else:self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
    class TCP(socketserver.BaseRequestHandler):
        def handle(self):
            while True:
                data=self.request.recv(4096)
                if not data:return
                self.request.sendall(data)
    class UDP(socketserver.BaseRequestHandler):
        def handle(self):self.request[1].sendto(self.request[0],self.client_address)
    servers=[]
    for port,label in [(8880,None),(8881,'valid'),(8884,'expired')]:
        server=http.server.ThreadingHTTPServer(('127.0.0.1',port),Handler)
        if label:
            context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.load_cert_chain(folder/(label+'.pem'),folder/'test-key.pem')
            server.socket=context.wrap_socket(server.socket,server_side=True)
        servers.append(server)
    for port,klass,handler in [(8882,socketserver.ThreadingUDPServer,UDP),(8883,socketserver.ThreadingTCPServer,TCP)]:
        klass.allow_reuse_address=True;server=klass(('127.0.0.1',port),handler);server.daemon_threads=True;servers.append(server)
    for server in servers:threading.Thread(target=server.serve_forever,daemon=True).start()
    digest=hashlib.sha256(payload).hexdigest()
    script=f'''#!/bin/sh
set -eu
curl --version
curl -fsSL --max-time 30 http://10.0.2.2:8880/redirect -o /work/payload
echo '{digest}  /work/payload' | sha256sum -c -
echo PASS_HTTP_REDIRECT_BINARY
curl -fsS --max-time 30 http://10.0.2.2:8880/chunked -o /work/payload
echo '{digest}  /work/payload' | sha256sum -c -
echo PASS_HTTP_CHUNKED
curl -fsS --max-time 30 --cacert /work/test-ca.pem https://10.0.2.2:8881/source.c -o /work/fetched.c
gcc -static /work/fetched.c -o /work/fetched
/work/fetched
echo PASS_TLS_DOWNLOAD_COMPILE
set +e
curl -fsS --max-time 10 https://10.0.2.2:8881/source.c -o /work/rejected
code=$?
set -e
test "$code" = 60
echo PASS_TLS_UNTRUSTED_REJECTED
set +e
curl -fsS --max-time 10 --cacert /work/test-ca.pem --resolve wrong.test:8881:10.0.2.2 https://wrong.test:8881/source.c -o /work/rejected
code=$?
set -e
test "$code" = 60
echo PASS_TLS_HOSTNAME_REJECTED
set +e
curl -fsS --max-time 10 --cacert /work/test-ca.pem https://10.0.2.2:8884/source.c -o /work/rejected
code=$?
set -e
test "$code" = 60
echo PASS_TLS_EXPIRED_REJECTED
gcc -static /work/network.c -o /work/network
/work/network
curl -fsS --max-time 30 https://example.com/ -o /work/example.html
grep 'Example Domain' /work/example.html
echo PASS_PUBLIC_HTTPS_DNS
curl -fsSL --max-time 120 https://curl.se/download/curl-8.22.0.tar.gz -o /work/curl-source.tar.gz
echo 'd54dd598bf05927a726deb38df31c6a255ba83ff1de57c5d1464dac3ed8f44a1  /work/curl-source.tar.gz' | sha256sum -c -
echo PASS_PUBLIC_PINNED_SOURCE_DOWNLOAD
sync
echo AURORA_NETWORK_TEST_COMPLETE
'''
    return servers,{'/work/test-ca.pem':ca.public_bytes(pem),'/work/net-test.sh':script.encode(),'/work/network.c':Path('tests/native-network.c').read_bytes()}
