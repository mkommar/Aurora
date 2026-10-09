"""Serve hash-verified native package sources to an Aurora QEMU guest."""
from functools import partial
import hashlib
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import threading
from pathlib import Path


class NativePackageMirror:
    """A loopback HTTP server exposed to Aurora through QEMU user NAT."""

    def __init__(self, root, package_lock, names, port=0):
        self.root = Path(root).resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self._verify_sources(package_lock, names)
        handler = partial(SimpleHTTPRequestHandler, directory=str(self.root))
        self.server = ThreadingHTTPServer(('127.0.0.1', port), handler)
        self.thread = None

    def _verify_sources(self, package_lock, names):
        missing = []
        mismatched = []
        for name in names:
            meta = package_lock[name]
            path = self.root / meta['archive']
            if not path.is_file():
                missing.append(str(path))
                continue
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            if digest != meta['sha256']:
                mismatched.append(f'{path}: expected {meta["sha256"]}, got {digest}')
        if missing or mismatched:
            details = []
            if missing:
                details.append('missing verified mirror inputs: ' + ', '.join(missing))
            if mismatched:
                details.append('mirror hash mismatch: ' + '; '.join(mismatched))
            raise FileNotFoundError('; '.join(details))

    @property
    def host_port(self):
        return self.server.server_address[1]

    def guest_url(self, guest_port):
        return f'http://10.0.2.2:{guest_port}'

    def start(self):
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def stop(self):
        self.server.shutdown()
        self.server.server_close()
        if self.thread:
            self.thread.join(timeout=5)
