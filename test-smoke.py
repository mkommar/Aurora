"""Integration checks against a running Aurora VM (run.ps1 -Headless).
Usage: python test-smoke.py --nm PATH_TO_LLVM_NM
Requires Python 3 and Pillow for screenshots. No third-party kernel code.
"""
import argparse, importlib.util, json, re, struct, subprocess, time
from pathlib import Path

spec = importlib.util.spec_from_file_location('qmp', 'tools-qmp.py')
mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
parser = argparse.ArgumentParser(); parser.add_argument('--nm', required=True)
parser.add_argument('--build-dir', default='build')
args = parser.parse_args()
build = Path(args.build_dir)
symbols = {}
for line in subprocess.check_output([args.nm, '-n', str(build/'desktop.elf')], text=True).splitlines():
    fields = line.split()
    if len(fields) == 3: symbols[fields[2]] = int(fields[0], 16)
q = mod.QMP()
results = []
def check(label, condition):
    assert condition, label
    results.append(label); print('PASS:', label)
def read(name, size=4):
    path = (build/'desktop-memory-probe.bin').resolve()
    # The desktop's 0x400000 virtual image has private backing at 32 MiB.
    q.call('pmemsave', {'val':symbols[name]-0x400000+0x2000000, 'size':size, 'filename':str(path)})
    return path.read_bytes()
def integer(name): return struct.unpack('<i', read(name))[0]
def enter(s):
    for c in s: q.key('spc' if c == ' ' else c)
    q.key('ret'); time.sleep(.15)
def move(x, y):
    # Read the guest cursor to send relative PS/2 movements without host acceleration.
    for _ in range(30):
        dx, dy = x-integer('mx'), y-integer('my')
        if not dx and not dy: return
        dx=max(-100,min(100,dx)); dy=max(-100,min(100,dy))
        q.call('input-send-event', {'events':[
            {'type':'rel','data':{'axis':'x','value':dx}},
            {'type':'rel','data':{'axis':'y','value':dy}}]})
        time.sleep(.05)
    raise AssertionError('Mouse movement did not reach target')
def button(down):
    q.call('input-send-event', {'events':[{'type':'btn','data':{'button':'left','down':down}}]})
    time.sleep(.12)

check('VM is running', q.call('query-status')['running'])
registers=q.call('human-monitor-command', {'command-line':'info registers'})
efer=int(re.search(r'EFER=([0-9a-fA-F]+)',registers).group(1),16)
check('CPU executes in 64-bit long mode with NX enabled', 'CS64' in registers and efer&0xd00==0xd00)
q.key('f2'); enter('help')
check('Terminal receives keyboard command', b'COMMAND help' in (build/'serial.log').read_bytes())
check('Terminal selected', integer('app') == 1)
enter('clear'); check('Clear command resets output', integer('linecount') == 0)
enter('about'); check('About command produces output', integer('linecount') == 3)
q.capture('test-terminal')
q.key('f3'); old=integer('nlen')
q.key('a'); q.key('b'); q.key('backspace')
check('Notes supports typing and backspace', integer('nlen') == old+1 and read('notes',old+2)[old:] == b'a\0')
q.key('f1'); q.key('f3')
check('Notes retained across app switches', integer('nlen') == old+1)
q.capture('test-notes')
q.key('f4'); before=integer('theme')
move(integer('wx')+80, integer('wy')+150); button(True); button(False)
check('Mouse changes palette', integer('theme') != before)
oldx,oldy=integer('wx'),integer('wy')
move(oldx+100,oldy+15); button(True); move(oldx+140,oldy+45); button(False)
check('Title bar drag moves window', integer('wx')==oldx+40 and integer('wy')==oldy+30)
q.capture('test-settings')
move(integer('wx')+615,integer('wy')+15);button(True);button(False)
check('Window close works', integer('app') == -1)
move(290,725);button(True);button(False)
check('Dock reopens welcome', integer('app') == 0)
q.capture('test-desktop')
(build/'test-results.json').write_text(json.dumps({'passed':results},indent=2))
print(f'{len(results)} integration checks passed.')
