"""Record a real QEMU test session, then render captioned social video variants.

Uses QMP screen capture (no host desktop capture), the existing integration
tests, Pillow for editorial overlays, and FFmpeg for H.264 encoding.
The overlays are explicitly labeled HOST TEST RUNNER; they are not guest UI.
"""
import argparse, importlib.util, json, os, socket, struct, subprocess, sys
import threading, time
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT=Path(__file__).resolve().parent
os.chdir(ROOT)
sys.path.insert(0,str(ROOT/'tools/media-python'))
import imageio_ffmpeg
FFMPEG=imageio_ffmpeg.get_ffmpeg_exe()
OUT=ROOT/'social'; SCRATCH=ROOT/'build/social'
OUT.mkdir(exist_ok=True); SCRATCH.mkdir(parents=True,exist_ok=True)
FPS=15
NM=Path('C:/Program Files/Unity/Hub/Editor/6000.4.0f1/Editor/Data/PlaybackEngines/AndroidPlayer/NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-nm.exe')
CREATE_NO_WINDOW=0x08000000

class QMP:
    def __init__(self,port):
        for attempt in range(100):
            try:self.s=socket.create_connection(('127.0.0.1',port),timeout=5);break
            except OSError:time.sleep(.1)
        else:raise RuntimeError('QEMU QMP did not start')
        self.f=self.s.makefile('rwb');self.f.readline();self.call('qmp_capabilities')
    def call(self,name,args=None):
        msg={'execute':name}
        if args is not None:msg['arguments']=args
        self.f.write((json.dumps(msg)+'\n').encode());self.f.flush()
        while True:
            reply=json.loads(self.f.readline())
            if 'error' in reply:raise RuntimeError(reply)
            if 'return' in reply:return reply['return']
    def close(self):self.f.close();self.s.close()
    def key(self,key,delay=.12):
        self.call('send-key',{'keys':[{'type':'qcode','data':key}],'hold-time':40})
        time.sleep(delay)
    def type(self,words):
        names={' ':'spc','.':'dot','-':'minus'}
        for c in words:self.key(names.get(c,c),.10)
    def read(self,addr,size):
        path=SCRATCH/'control-memory.bin'
        self.call('pmemsave',{'val':addr,'size':size,'filename':str(path)})
        return path.read_bytes()
    def move(self,x,y,symbols):
        for _ in range(80):
            mx=struct.unpack('<i',self.read(symbols['mx']-0x400000+0x2000000,4))[0]
            my=struct.unpack('<i',self.read(symbols['my']-0x400000+0x2000000,4))[0]
            dx=max(-18,min(18,x-mx));dy=max(-18,min(18,y-my))
            if not dx and not dy:return
            self.call('input-send-event',{'events':[
                {'type':'rel','data':{'axis':'x','value':dx}},
                {'type':'rel','data':{'axis':'y','value':dy}}]})
            time.sleep(.025)
        raise RuntimeError('Mouse failed to reach destination')
    def button(self,down):
        self.call('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':down}}]})
        time.sleep(.16)

def symbols(file):
    result={}
    for line in subprocess.check_output([str(NM),'-n',str(file)],text=True).splitlines():
        f=line.split()
        if len(f)==3:result[f[2]]=int(f[0],16)
    return result

def encoder(path,w,h):
    log=open(SCRATCH/(path.stem+'-ffmpeg.log'),'w')
    proc=subprocess.Popen([FFMPEG,'-hide_banner','-loglevel','error','-y',
        '-f','rawvideo','-pix_fmt','rgb24','-s',f'{w}x{h}','-r',str(FPS),'-i','-',
        '-an','-c:v','libx264','-preset','veryfast','-crf','18','-pix_fmt','yuv420p',
        '-movflags','+faststart',str(path)],stdin=subprocess.PIPE,stderr=log,creationflags=CREATE_NO_WINDOW)
    return proc,log

def record():
    # Never attach to or stop an unrelated QEMU instance during reproduction.
    for port in (4444,4445):
        with socket.socket() as probe:
            probe.setsockopt(socket.SOL_SOCKET,socket.SO_EXCLUSIVEADDRUSE,1)
            try:probe.bind(('127.0.0.1',port))
            except OSError:raise RuntimeError(f'Port {port} is in use; close the other QEMU instance first.')
    # Rebuild the test bundle so metadata/probes match the recorded kernel.
    subprocess.run(['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File',
                    str(ROOT/'build.ps1'),'-SelfTest'],check=True)
    ks=symbols(ROOT/'build/selftest/kernel.elf');ds=symbols(ROOT/'build/selftest/desktop.elf')
    serial=ROOT/'build/selftest/serial.log'
    qemu=subprocess.Popen([str(ROOT/'tools/qemu/qemu-system-x86_64.exe'),
        '-name','Aurora recording','-machine','pc','-accel','tcg','-cpu','qemu64',
        '-m','128M','-vga','std','-drive','format=raw,file=build/selftest/aurora.img',
        '-serial','file:build/selftest/serial.log','-net','none','-display','none','-S',
        '-qmp','tcp:127.0.0.1:4444,server=on,wait=off',
        '-qmp','tcp:127.0.0.1:4445,server=on,wait=off'],creationflags=CREATE_NO_WINDOW)
    capture=QMP(4445);control=QMP(4444)
    video,video_log=encoder(OUT/'aurora-qemu-original.mp4',1024,768)
    stop=threading.Event();failure=[];scenes=[];samples=[];start=time.perf_counter()
    def scene(name):
        scenes.append({'time':time.perf_counter()-start,'name':name})
        print('RECORDING:',name,flush=True)
    def capture_loop():
        written=0;last_sample=-1
        try:
            while not stop.is_set():
                capture.call('screendump',{'filename':str(SCRATCH/'frame.ppm')})
                im=Image.open(SCRATCH/'frame.ppm').convert('RGB')
                # Firmware text modes can start at a different resolution.
                if im.size!=(1024,768):im=im.resize((1024,768))
                raw=im.tobytes();elapsed=time.perf_counter()-start
                target=int(elapsed*FPS)+1
                while written<target:video.stdin.write(raw);written+=1
                if elapsed-last_sample>=.5:
                    snapshot={}
                    for name,offset in [('timer_ticks',0),('ipc_messages',0),('task_preemptions',6*8)]:
                        p=SCRATCH/'capture-memory.bin'
                        capture.call('pmemsave',{'val':ks[name]+offset,'size':8,'filename':str(p)})
                        snapshot[name]=struct.unpack('<Q',p.read_bytes())[0]
                    samples.append({'time':elapsed,**snapshot});last_sample=elapsed
                delay=written/FPS-(time.perf_counter()-start)
                if delay>0:stop.wait(delay)
        except Exception as e:failure.append(repr(e))
        finally:
            video.stdin.close();video.wait();video_log.close();capture.close()
    thread=threading.Thread(target=capture_loop);thread.start()
    def run_test(script,*args):
        nonlocal control
        control.close();control=None
        proc=subprocess.run([sys.executable,script,'--nm',str(NM),*args],capture_output=True,text=True)
        (OUT/(Path(script).stem+'-output.txt')).write_text(proc.stdout+proc.stderr)
        print(proc.stdout,flush=True)
        if proc.returncode:raise RuntimeError(proc.stderr or 'Test failed')
        control=QMP(4444)
    try:
        scene('boot');control.call('cont')
        deadline=time.monotonic()+15
        while 'desktop ready' not in serial.read_text(errors='replace'):
            if time.monotonic()>deadline:raise RuntimeError('Guest did not boot')
            time.sleep(.1)
        time.sleep(4)
        scene('isolation');run_test('test-microkernel.py');time.sleep(5)
        scene('gui');run_test('test-smoke.py','--build-dir','build/selftest');time.sleep(2)
        scene('notes');control.key('f3');control.key('backspace');control.key('ret')
        control.type('still typing after six isolated faults.');time.sleep(3)
        scene('mouse');control.key('f4');time.sleep(1)
        # Smoke test moved the window to (230,148).
        control.move(310,298,ds);control.button(True);control.button(False);time.sleep(1)
        control.move(370,165,ds);control.button(True);control.move(310,135,ds);control.button(False)
        time.sleep(2)
        scene('preemption');control.key('f2');control.type('clear');control.key('ret')
        control.type('mem');control.key('ret');time.sleep(6)
        scene('result');control.key('f1');time.sleep(6)
    finally:
        stop.set();thread.join(timeout=20)
        if control:
            try:control.call('quit')
            except OSError:pass
            control.close()
        if qemu.poll() is None:qemu.terminate()
        qemu.wait(timeout=10)
    if failure or video.returncode:raise RuntimeError(f'Capture failed: {failure}, encoder={video.returncode}')
    evidence={'scenes':scenes,'samples':samples,'fps':FPS,'duration':time.perf_counter()-start,
              'capture':'Live QEMU QMP screendump, real-time; editorial overlays added separately.'}
    (OUT/'capture-timeline.json').write_text(json.dumps(evidence,indent=2))
    for name in ['serial.log','microkernel-results.json','test-results.json']:
        (OUT/name).write_bytes((ROOT/'build/selftest'/name).read_bytes())
    return evidence

MINT='#55e0c2';WHITE='#edf4f7';MUTED='#99aeba';BG='#0a1722';PANEL='#132a39'
def font(size,bold=False):return ImageFont.truetype('C:/Windows/Fonts/segoeuib.ttf' if bold else 'C:/Windows/Fonts/segoeui.ttf',size)
FONTS={}
def ft(size,bold=False):
    key=(size,bold)
    if key not in FONTS:FONTS[key]=font(size,bold)
    return FONTS[key]

COPY={
 'boot':('An original OS.\nA microkernel underneath.', 'BOOTING AURORA 0.2',
         ['64-bit original kernel','Desktop, input and display in user mode','Real QEMU capture starts here']),
 'isolation':('Six faults.\nThe desktop keeps running.', 'FAULT ISOLATION',
         ['Kernel and foreign-memory writes blocked','Direct I/O and code writes blocked','Stack execution and invalid opcode trapped']),
 'gui':('Now test\nthe desktop.', 'GUI REGRESSION TESTS',
         ['Keyboard input and terminal commands','Notes editing and RAM retention','Mouse clicks, dragging and app switching']),
 'notes':('Input still\nreaches the apps.', 'LIVE NOTES DEMO',
         ['Typing through the input service','Messages delivered across address spaces','Notes remain in RAM until reboot']),
 'mouse':('A working desktop.\nAcross process boundaries.', 'LIVE MOUSE DEMO',
         ['Change the palette','Drag the application window','Display presents frames through IPC']),
 'preemption':('An infinite loop\nstill loses the CPU.', 'PREEMPTIVE SCHEDULING',
         ['Test process 6 spins without yielding','Timer interrupts force context switches','The desktop remains responsive']),
 'result':('40 checks passed.', 'TEST SESSION COMPLETE',
         ['28 microkernel checks','12 desktop integration checks','A small, original OS — still growing']),
}

def compose(frame,scene,sample,progress,vertical):
    size=(1080,1920) if vertical else (1920,1080)
    im=Image.new('RGB',size,BG);d=ImageDraw.Draw(im)
    title,kicker,bullets=COPY[scene]
    if vertical:
        d.text((44,44),'AURORA OS  /  0.2',font=ft(29,True),fill=MINT)
        d.text((44,103),title,font=ft(57,True),fill=WHITE,spacing=3)
        d.text((44,285),'ORIGINAL x86-64 MICROKERNEL',font=ft(25),fill=MUTED)
        d.rounded_rectangle((28,355,1052,1187),radius=15,fill=PANEL)
        d.text((49,368),'QEMU  /  LIVE GUEST DISPLAY',font=ft(22,True),fill=MUTED)
        im.paste(frame,(28,412))
        d.rounded_rectangle((28,1215,1052,1770),radius=15,fill=PANEL)
        d.text((52,1240),'HOST TEST RUNNER  /  '+kicker,font=ft(22,True),fill=MINT)
        for i,line in enumerate(bullets):
            d.text((52,1300+i*62),line,font=ft(32,True),fill=WHITE)
        if scene in ('preemption','result'):
            d.text((52,1513),f"Forced preemptions of process 6: {sample.get('task_preemptions',0):,}",font=ft(31),fill=MINT)
            d.text((52,1575),f"Timer ticks: {sample.get('timer_ticks',0):,}   |   IPC messages: {sample.get('ipc_messages',0):,}",font=ft(28),fill=MUTED)
        elif scene in ('isolation','gui','notes','mouse'):
            d.text((52,1540),'Six deliberate faults are confined to test tasks.',font=ft(29),fill=MUTED)
            d.text((52,1595),'A non-yielding test process remains active.',font=ft(29),fill=MUTED)
        d.text((44,1803),'REAL CAPTURE  /  SELF-TEST BUILD  /  NO AUDIO',font=ft(22),fill=MUTED)
        d.rectangle((44,1862,1036,1868),fill='#284353')
        d.rectangle((44,1862,44+int(992*progress),1868),fill=MINT)
    else:
        d.text((48,35),'AURORA OS  /  0.2',font=ft(27,True),fill=MINT)
        d.text((48,81),title.replace('\n',' '),font=ft(48,True),fill=WHITE)
        d.rounded_rectangle((32,195,1088,1035),radius=14,fill=PANEL)
        d.text((49,209),'QEMU  /  LIVE GUEST DISPLAY',font=ft(21,True),fill=MUTED)
        im.paste(frame,(48,255))
        d.text((1130,223),'HOST TEST RUNNER',font=ft(23,True),fill=MINT)
        d.text((1130,278),kicker,font=ft(29,True),fill=WHITE)
        # Wrap the editorial copy; guest screen remains uncropped.
        y=365
        for line in bullets:
            words=line.split();rows=['']
            for word in words:
                candidate=(rows[-1]+' '+word).strip()
                if d.textlength(candidate,font=ft(29))>710:rows.append(word)
                else:rows[-1]=candidate
            for row in rows:d.text((1130,y),row,font=ft(29),fill=WHITE);y+=42
            y+=22
        if scene in ('preemption','result'):
            d.text((1130,735),f"Process 6 preemptions: {sample.get('task_preemptions',0):,}",font=ft(31,True),fill=MINT)
            d.text((1130,789),f"Timer ticks: {sample.get('timer_ticks',0):,}",font=ft(28),fill=MUTED)
            d.text((1130,834),f"IPC messages: {sample.get('ipc_messages',0):,}",font=ft(28),fill=MUTED)
        else:
            d.text((1130,795),'Original kernel. Three user-mode services.',font=ft(26),fill=MUTED)
            d.text((1130,842),'Captured directly from QEMU.',font=ft(26),fill=MUTED)
        d.text((1130,946),'SELF-TEST BUILD  /  NO AUDIO',font=ft(22),fill=MUTED)
        d.rectangle((1130,1008,1870,1014),fill='#284353')
        d.rectangle((1130,1008,1130+int(740*progress),1014),fill=MINT)
    return im

def render(evidence):
    import re
    source=OUT/'aurora-qemu-original.mp4'
    info=subprocess.run([FFMPEG,'-hide_banner','-i',str(source)],capture_output=True,text=True).stderr
    (OUT/'source-video-info.txt').write_text(info)
    decoder=subprocess.Popen([FFMPEG,'-loglevel','error','-i',str(source),'-f','rawvideo','-pix_fmt','rgb24','-'],stdout=subprocess.PIPE,creationflags=CREATE_NO_WINDOW)
    v,vl=encoder(OUT/'aurora-tests-vertical.mp4',1080,1920)
    h,hl=encoder(OUT/'aurora-tests-landscape.mp4',1920,1080)
    scenes=evidence['scenes'];samples=evidence['samples'];si=0;ti=0;n=0;poster=False
    duration=evidence['duration']
    try:
        while True:
            raw=decoder.stdout.read(1024*768*3)
            if not raw:break
            if len(raw)!=1024*768*3:raise RuntimeError('Truncated source frame')
            stamp=n/FPS
            while si+1<len(scenes) and scenes[si+1]['time']<=stamp:si+=1
            while ti+1<len(samples) and samples[ti+1]['time']<=stamp:ti+=1
            name=scenes[si]['name'];sample=samples[ti] if samples else {}
            frame=Image.frombytes('RGB',(1024,768),raw)
            vertical=compose(frame,name,sample,min(1,stamp/duration),True)
            horizontal=compose(frame,name,sample,min(1,stamp/duration),False)
            v.stdin.write(vertical.tobytes());h.stdin.write(horizontal.tobytes())
            if name=='result' and stamp>scenes[si]['time']+1 and not poster:
                vertical.save(OUT/'aurora-poster.jpg',quality=95)
                horizontal.save(OUT/'aurora-landscape-poster.jpg',quality=95)
                poster=True
            if n%150==0:print(f'RENDERED {stamp:.0f}s',flush=True)
            n+=1
    finally:
        decoder.stdout.close();decoder.wait();v.stdin.close();h.stdin.close()
        v.wait();h.wait();vl.close();hl.close()
    if decoder.returncode or v.returncode or h.returncode:raise RuntimeError('Video render failed')
    print(f'FINISHED: {n} frames / {n/FPS:.2f}s',flush=True)
    evidence['encoded_frames']=n;evidence['encoded_duration']=n/FPS
    (OUT/'capture-timeline.json').write_text(json.dumps(evidence,indent=2))

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--render-only',action='store_true');args=parser.parse_args()
    evidence=json.loads((OUT/'capture-timeline.json').read_text()) if args.render_only else record()
    render(evidence)
