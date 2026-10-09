"""Verify that AMD-Vi hardware without IVRS refuses DMA-backed startup."""
import argparse, subprocess, time
from pathlib import Path
from qemu_iommu import write_ivrs

parser = argparse.ArgumentParser()
parser.add_argument('--qemu', default='qemu-system-x86_64')
parser.add_argument('--build-dir', default='build')
args = parser.parse_args(); build = Path(args.build_dir)
invalid = build / 'qemu-invalid-ivrs.bin'; write_ivrs(invalid); data = bytearray(invalid.read_bytes()); data[56:64] = (1).to_bytes(8, 'little'); invalid.write_bytes(data)
for label, table in [('without IVRS', None), ('with invalid IVRS', invalid)]:
    serial = build / ('amd-iommu-disabled.log' if table is None else 'amd-iommu-invalid.log')
    command = [args.qemu, '-machine', 'q35,accel=tcg', '-m', '128M', '-device', 'amd-iommu,pt=off']
    if table is not None: command += ['-acpitable', f'file={table}']
    command += ['-drive', f'format=raw,file={build / "aurora.img"}', '-display', 'none', '-net', 'none', '-serial', f'file:{serial}', '-no-reboot']
    with serial.open('w'):
        process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        deadline = time.time() + 10
        while time.time() < deadline:
            text = serial.read_text(errors='replace')
            if 'KERNEL PANIC' in text: break
            time.sleep(.1)
        text = serial.read_text(errors='replace')
        assert 'KERNEL PANIC: DMA isolation unavailable' in text, text[-4000:]
        assert 'VIRTIO: PCI block queue ready' not in text
        print(f'PASS: AMD-Vi startup is refused {label}')
    finally:
        process.terminate()
        try: process.wait(timeout=5)
        except subprocess.TimeoutExpired: process.kill(); process.wait()
