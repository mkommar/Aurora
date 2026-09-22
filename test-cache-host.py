"""Run production cache eviction code with pinned/open/recovery fixtures."""
import ctypes
from pathlib import Path
import subprocess

llvm=Path(r'C:/Program Files/Unity/Hub/Editor/6000.4.0f1/Editor/Data/PlaybackEngines/AndroidPlayer/NDK/toolchains/llvm/prebuilt/windows-x86_64/bin')
out=Path('build/cache-host-tests');out.mkdir(parents=True,exist_ok=True)
subprocess.run([str(llvm/'clang.exe'),'--target=x86_64-pc-windows-msvc','-ffreestanding','-fno-builtin','-fno-stack-protector','-mno-stack-arg-probe','-O2','-Wall','-Wextra','-Werror','-c','tests/cache-host.c','-o',str(out/'cache.obj')],check=True)
subprocess.run([str(llvm/'ld.lld.exe'),'-flavor','link','/dll','/noentry','/nodefaultlib','/out:'+str(out/'cache.dll'),str(out/'cache.obj')],check=True)
result=ctypes.CDLL(str((out/'cache.dll').resolve())).cache_test()
assert result==0,f'Cache fixture {result} failed'
print('PASS raw directory preservation, open descriptor pins, recovery pins, eviction and tail reclamation')
