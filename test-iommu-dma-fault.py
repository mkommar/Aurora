"""Exercise a real VirtIO DMA transaction outside its hardware domain.

The image must be built with --self-test --dma-fault-test. This harness never
uses the software DMA validator to manufacture a fault: the guest publishes a
descriptor containing an unauthorized physical address and QEMU's VirtIO
device performs the resulting transaction through its IOMMU model.
"""
import argparse
import re
import subprocess
import time
from pathlib import Path
from qemu_iommu import write_dmar, write_ivrs

parser = argparse.ArgumentParser()
parser.add_argument('--qemu', default='qemu-system-x86_64')
parser.add_argument('--build-dir', default='build/selftest')
parser.add_argument('--backend', choices=['intel', 'amd', 'both'], default='both')
args = parser.parse_args()
build = Path(args.build_dir)
markers = {
    'blocked': 'IOMMU TEST: unauthorized VirtIO DMA blocked; fault latched; device quarantined',
    'stale': 'IOMMU TEST: stale completion rejected after quarantine',
}

def run(backend):
    serial = build / f'{backend}-dma-fault-serial.log'
    table = build / f'qemu-{backend}-dma-fault.bin'
    development = build / f'{backend}-dma-fault-development.img'
    if backend == 'intel':
        write_dmar(table)
        iommu = ['-device', 'intel-iommu,intremap=on,dma-translation=on,aw-bits=48', '-acpitable', f'file={table}']
    else:
        write_ivrs(table)
        iommu = ['-device', 'amd-iommu,pt=off', '-acpitable', f'file={table}']
    if not development.exists():
        development.write_bytes(b'\0' * (16 * 1024 * 1024))
    command = [args.qemu, '-machine', 'q35,accel=tcg', '-m', '256M', *iommu,
               '-drive', f'format=raw,file={build / "aurora.img"}',
               '-drive', f'format=raw,file={development},if=none,id=development',
               '-device', 'virtio-blk-pci,drive=development,disable-legacy=on,iommu_platform=on',
               '-display', 'none', '-net', 'none', '-serial', f'file:{serial}', '-no-reboot']
    with serial.open('w') as output:
        process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        deadline = time.time() + 20
        while time.time() < deadline:
            text = serial.read_text(errors='replace') if serial.exists() else ''
            if markers['blocked'] in text or markers['stale'] in text or 'KERNEL PANIC' in text:
                break
            time.sleep(.1)
        text = serial.read_text(errors='replace')
        expected = f'IOMMU: {"AMD-Vi" if backend == "amd" else "Intel VT-d"} enabled'
        marker = markers['blocked']
        if expected not in text:
            print(f'BLOCKED: QEMU {backend} did not boot the expected IOMMU backend')
            print(text[-4000:])
            return False
        if marker not in text:
            print(f'BLOCKED: QEMU {backend} did not latch the injected DMA fault')
            print(text[-4000:])
            return False
        fault = re.search(r'device quarantined source=([0-9a-f]+) address=([0-9a-f]+)', text)
        if not fault or int(fault.group(1), 16) == 0 or int(fault.group(2), 16) != 0x06000000:
            print(f'BLOCKED: QEMU {backend} did not report the injected source and target')
            print(text[-4000:])
            return False
        if markers['stale'] not in text:
            print(f'BLOCKED: QEMU {backend} did not reject a stale completion after quarantine')
            print(text[-4000:])
            return False
        if 'KERNEL PANIC' in text:
            print(f'FAIL: QEMU {backend} panicked during the DMA-fault test')
            print(text[-4000:])
            return False
        print(f'PASS: QEMU {backend} malicious VirtIO DMA was blocked and quarantined')
        return True
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait()

results = [run(backend) for backend in (['intel', 'amd'] if args.backend == 'both' else [args.backend])]
if not all(results):
    raise SystemExit(1)
