"""Verify that a QEMU guest without Intel VT-d refuses DMA-backed startup."""
import argparse, subprocess, time
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--qemu', default='qemu-system-x86_64')
parser.add_argument('--build-dir', default='build')
args = parser.parse_args()
build = Path(args.build_dir)
serial = build / 'iommu-disabled-serial.log'
with serial.open('w') as output:
    process = subprocess.Popen([
        args.qemu, '-machine', 'pc,accel=tcg', '-m', '128M',
        '-drive', f'format=raw,file={build / "aurora.img"}', '-display', 'none',
        '-net', 'none', '-serial', f'file:{serial}', '-no-reboot'], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
try:
    deadline = time.time() + 10
    while time.time() < deadline:
        text = serial.read_text(errors='replace')
        if 'KERNEL PANIC' in text: break
        time.sleep(.1)
    text = serial.read_text(errors='replace')
    assert 'KERNEL PANIC: DMA isolation unavailable' in text, text[-4000:]
    assert 'VIRTIO: PCI block queue ready' not in text
    print('PASS: DMA-backed startup is refused without an ACPI DMAR/VT-d unit')
finally:
    process.terminate()
    try: process.wait(timeout=5)
    except subprocess.TimeoutExpired: process.kill(); process.wait()
