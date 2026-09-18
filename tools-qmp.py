"""Local QEMU inspection and input; only connects to Aurora's loopback QMP port."""
import json, socket, sys, time
from pathlib import Path

class QMP:
    def __init__(self, port=4444):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=5)
        self.f = self.sock.makefile('rwb')
        self.f.readline()
        self.call('qmp_capabilities')
    def call(self, cmd, args=None):
        msg = {'execute': cmd}
        if args is not None: msg['arguments'] = args
        self.f.write((json.dumps(msg)+'\n').encode()); self.f.flush()
        while True:
            reply = json.loads(self.f.readline())
            if 'error' in reply: raise RuntimeError(reply)
            if 'return' in reply: return reply['return']
    def key(self, key):
        self.call('send-key', {'keys':[{'type':'qcode','data':key}], 'hold-time':30})
        time.sleep(.07)
    def capture(self, name):
        from PIL import Image
        path = Path('build', name).resolve()
        self.call('screendump', {'filename':str(path.with_suffix('.ppm'))})
        Image.open(path.with_suffix('.ppm')).save(path.with_suffix('.png'))
        print(path.with_suffix('.png'))

if __name__ == '__main__':
    q = QMP()
    if sys.argv[1] == 'capture': q.capture(sys.argv[2])
    elif sys.argv[1] == 'key':
        for k in sys.argv[2:]: q.key(k)
    elif sys.argv[1] == 'quit': q.call('quit')
    elif sys.argv[1] == 'status':
        print(q.call('query-status'))
        print(q.call('human-monitor-command', {'command-line':'info registers'}))
