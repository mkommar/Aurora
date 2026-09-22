"""Fault-inject delayed interrupts and incomplete requests in production code."""
import ctypes
from pathlib import Path
import subprocess

llvm=Path(r'C:/Program Files/Unity/Hub/Editor/6000.4.0f1/Editor/Data/PlaybackEngines/AndroidPlayer/NDK/toolchains/llvm/prebuilt/windows-x86_64/bin')
out=Path('build/virtio-completion-tests');out.mkdir(parents=True,exist_ok=True)
subprocess.run([str(llvm/'clang.exe'),'--target=x86_64-pc-windows-msvc','-ffreestanding','-fno-builtin','-fno-stack-protector','-mno-stack-arg-probe','-O2','-Wall','-Wextra','-Werror','-c','tests/virtio-completion-host.c','-o',str(out/'completion.obj')],check=True)
subprocess.run([str(llvm/'ld.lld.exe'),'-flavor','link','/dll','/noentry','/nodefaultlib','/out:'+str(out/'completion.dll'),str(out/'completion.obj')],check=True)
result=ctypes.CDLL(str((out/'completion.dll').resolve())).completion_test()
assert result==0,f'Completion fixture {result} failed'
print('PASS late completion, real timeout, counter wrap, partial batch, MSI-X and INTx')
