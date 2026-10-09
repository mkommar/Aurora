"""Host regression for the bounded storage-service transport seam."""
import shutil, subprocess
from pathlib import Path

cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
if not cc:
    raise SystemExit("no host C compiler available")
out = Path("build/storage-service-tests"); out.mkdir(parents=True, exist_ok=True)
binary = out / "storage-service-host"
subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I.",
                "tests/storage-service-host.c", "-o", str(binary)], check=True)
subprocess.run([str(binary)], check=True)
subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I.",
                "tests/virtio-pci-host.c", "-o", str(out / "virtio-pci-host")], check=True)
subprocess.run([str(out / "virtio-pci-host")], check=True)
