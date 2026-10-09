"""Boot Aurora with QEMU's AMD-Vi model and an injected IVRS/IVHD table."""
import argparse, subprocess, time
from pathlib import Path
from qemu_iommu import write_ivrs

parser = argparse.ArgumentParser()
parser.add_argument('--qemu', default='qemu-system-x86_64')
parser.add_argument('--build-dir', default='build')
args = parser.parse_args(); build = Path(args.build_dir)
serial = build / 'amd-iommu-serial.log'; ivrs = build / 'qemu-ivrs.bin'; development = build / 'amd-iommu-development.img'
write_ivrs(ivrs)
if not development.exists(): development.write_bytes(b'\0' * (16 * 1024 * 1024))
with serial.open('w'):
    process = subprocess.Popen([
        args.qemu, '-machine', 'q35,accel=tcg', '-m', '256M', '-device', 'amd-iommu,pt=off',
        '-acpitable', f'file={ivrs}', '-drive', f'format=raw,file={build / "aurora.img"}',
        '-drive', f'format=raw,file={development},if=none,id=development',
        '-device', 'virtio-blk-pci,drive=development,disable-legacy=on,iommu_platform=on', '-display', 'none',
        '-serial', f'file:{serial}', '-no-reboot'], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
try:
    deadline = time.time() + 20
    while time.time() < deadline:
        text = serial.read_text(errors='replace')
        if 'desktop ready' in text or 'KERNEL PANIC' in text: break
        time.sleep(.1)
    text = serial.read_text(errors='replace')
    assert 'IOMMU: AMD-Vi enabled' in text, text[-4000:]
    assert ('VIRTIO: PCI block queue ready' in text or
            'VIRTIO: modern PCI block queue ready' in text), text[-4000:]
    assert 'KERNEL PANIC' not in text, text[-4000:]
    print('PASS: QEMU AMD-Vi IVRS discovery, translation enable and VirtIO boot')
finally:
    process.terminate()
    try: process.wait(timeout=5)
    except subprocess.TimeoutExpired: process.kill(); process.wait()
