"""Reproduce AMD-Vi command-ring programming under QEMU 8.2.x.

This is diagnostic coverage, not a fault-success test. It records the AMD-Vi
register/control trace and reports whether QEMU consumes a command and advances
the ring. A missing command execution is an explicit blocked result.
"""
import argparse
import subprocess
import time
from pathlib import Path
from qemu_iommu import write_ivrs

parser = argparse.ArgumentParser()
parser.add_argument('--qemu', default='qemu-system-x86_64')
parser.add_argument('--build-dir', default='build/selftest')
parser.add_argument('--trace', default='build/selftest/amdvi-command-ring.trace')
parser.add_argument('--timeout', type=float, default=30.0)
args = parser.parse_args()

build = Path(args.build_dir).resolve()
serial = build / 'amdvi-command-ring-serial.log'
ivrs = build / 'qemu-amdvi-command-ring.bin'
trace = Path(args.trace).resolve()
write_ivrs(ivrs)
if not (build / 'amdvi-command-ring-development.img').exists():
    (build / 'amdvi-command-ring-development.img').write_bytes(b'\0' * (16 * 1024 * 1024))

events = ','.join((
    'amdvi_mmio_read', 'amdvi_mmio_write', 'amdvi_control_status',
    'amdvi_command_exec', 'amdvi_command_error', 'amdvi_unhandled_command',
    'amdvi_completion_wait', 'amdvi_completion_wait_fail',
    'amdvi_page_fault', 'amdvi_evntlog_fail',
))
event_file = build / 'amdvi-command-ring.events'
event_file.write_text('\n'.join(events.split(',')) + '\n')
command = [
    args.qemu, '-machine', 'q35,accel=tcg', '-m', '256M',
    '-device', 'amd-iommu,pt=off', '-acpitable', f'file={ivrs}',
    '-drive', f'format=raw,file={build / "aurora.img"}',
    '-drive', f'format=raw,file={build / "amdvi-command-ring-development.img"},if=none,id=development',
    '-device', 'virtio-blk-pci,drive=development,disable-legacy=on,iommu_platform=on',
    '-display', 'none', '-net', 'none', '-serial', f'file:{serial}', '-no-reboot',
    '-trace', f'events={event_file}', '-trace', f'file={trace}',
]

with serial.open('w'):
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
try:
    deadline = time.time() + args.timeout
    while time.time() < deadline:
        text = serial.read_text(errors='replace')
        if 'IOMMU: AMD-Vi enabled' in text or 'KERNEL PANIC' in text:
            break
        time.sleep(.1)
finally:
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()

stderr = process.stderr.read().decode(errors='replace') if process.stderr else ''

text = serial.read_text(errors='replace')
trace_text = trace.read_text(errors='replace') if trace.exists() else ''
if 'IOMMU: AMD-Vi enabled' not in text:
    print('BLOCKED: AMD-Vi did not reach guest backend enablement')
    print(text[-4000:])
    print(stderr[-4000:])
    print(trace_text[-4000:])
    raise SystemExit(1)
if 'amdvi_command_exec' not in trace_text:
    print('BLOCKED: QEMU AMD-Vi did not consume the command ring')
    print(trace_text[-4000:])
    raise SystemExit(1)
print('PASS: QEMU AMD-Vi consumed a command-ring entry')
