"""Regression checks for production and self-test flat-image boundaries."""
import tempfile
import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location('build_linux', Path(__file__).with_name('build-linux.py'))
build_linux = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_linux)

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    payload = root / 'hello.elf'
    payload.write_bytes(b'ELF' + b'\0' * 127)
    production = root / 'production.img'
    production.write_bytes(b'\0' * (16 * 1024 * 1024))
    build_linux.pack_files(production, {'hello': payload}, 512, 16 * 1024 * 1024)
    selftest = root / 'selftest.img'
    selftest.write_bytes(b'\0' * (32 * 1024 * 1024))
    build_linux.pack_files(selftest, {'hello': payload}, 1024, 32 * 1024 * 1024)
    data = selftest.read_bytes()
    assert data[1024 * 512:1024 * 512 + 8] == b'AURFS01\0'
    assert (1024 + 8 + 32 * 128) * 512 < len(data)
    assert (4608 + 307200) < 1024 * 512
print('PASS production and self-test image layout boundaries')
