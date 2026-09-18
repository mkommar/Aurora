"""Create a NEW ext2 development image, preserving the existing Aurora disk.

Uses the pinned lwext4 host image tool; never formats a physical drive.
"""
from pathlib import Path
import argparse,ctypes as C,struct,tarfile,posixpath,zlib,uuid
p=argparse.ArgumentParser();p.add_argument('--image',default='build/development.img');p.add_argument('--gnu',action='store_true');p.add_argument('--partitioned',action='store_true');args=p.parse_args()
target=Path(args.image);temporary=target.with_suffix('.img.partial')
if target.exists() or temporary.exists():raise SystemExit('Refusing to overwrite an existing image or partial image')
lib=C.CDLL(str(Path('build/image-tool/ext2-image.dll').resolve()))
callback_type=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_uint64,C.c_uint32,C.c_int)
lib.au_format.argtypes=[callback_type];lib.au_put.argtypes=[C.c_char_p,C.c_void_p,C.c_uint64,C.c_uint32]
lib.au_fatformat.argtypes=[callback_type]
lib.au_mkdir.argtypes=[C.c_char_p];lib.au_symlink.argtypes=[C.c_char_p,C.c_char_p]
def check(code,operation):
    if code:raise RuntimeError(f'{operation}: filesystem error {code}')
folders={'/'}
def host_path(path):
    for alias in ('include','lib','libexec','x86_64-linux-musl'):
        prefix='/usr/'+alias
        if path==prefix or path.startswith(prefix+'/'):return path[4:]
    return path
def mkdir(path):
    path=host_path(path)
    if path in folders:return
    mkdir(posixpath.dirname(path));code=lib.au_mkdir(path.encode())
    if code!=17:check(code,'mkdir '+path)
    folders.add(path)
def put(path,data,mode=0o644):
    path=host_path(path)
    mkdir(posixpath.dirname(path));check(lib.au_put(path.encode(),data,len(data),mode),'write '+path)
def symlink(path,target):
    mkdir(posixpath.dirname(path));check(lib.au_symlink(path.encode(),target.encode()),'symlink '+path)
target.parent.mkdir(parents=True,exist_ok=True)
def write_gpt(disk,sectors):
    entries=bytearray(128*128)
    for i,(kind,name,start,count) in enumerate([
        ('0fc63daf-8483-4772-8e79-3d69d8477de4','Aurora development',2048,1048576),
        ('ebd0a0a2-b9e5-4433-87c0-68b6b72699c7','Aurora exchange',1050624,262144)]):
        struct.pack_into('<16s16sQQQ72s',entries,i*128,uuid.UUID(kind).bytes_le,uuid.uuid5(uuid.NAMESPACE_DNS,name).bytes_le,start,start+count-1,0,name.encode('utf-16le'))
    mbr=bytearray(512);mbr[446:462]=struct.pack('<B3sB3sII',0,b'\0\2\0',0xee,b'\xff'*3,1,sectors-1);mbr[510:]=b'\x55\xaa';disk.seek(0);disk.write(mbr)
    def header(current,backup,table):
        data=bytearray(512);struct.pack_into('<8sIIIIQQQQ16sQIII',data,0,b'EFI PART',0x10000,92,0,0,current,backup,2048,sectors-34,uuid.uuid5(uuid.NAMESPACE_DNS,'Aurora disk').bytes_le,table,128,128,zlib.crc32(entries))
        struct.pack_into('<I',data,16,zlib.crc32(data[:92]));return data
    for sector,data in [(1,header(1,sectors-1,2)),(2,entries),(sectors-33,entries),(sectors-1,header(sectors-1,1,sectors-33))]:disk.seek(sector*512);disk.write(data)
with temporary.open('x+b') as disk:
    disk.truncate((1314816 if args.partitioned else 1048576)*512)
    if args.partitioned:write_gpt(disk,1314816)
    @callback_type
    def transfer(pointer,sector,count,write):
        try:
            if sector+count>1048576:return 5
            disk.seek((sector+(2048 if args.partitioned else 0))*512)
            if write:disk.write(C.string_at(pointer,count*512))
            else:
                data=disk.read(count*512)
                if len(data)!=count*512:return 5
                C.memmove(pointer,data,len(data))
            return 0
        except Exception:return 5
    check(lib.au_format(transfer),'format');check(lib.au_mount(),'mount')
    print('Copying existing development files into ext2...',flush=True)
    with Path('build/toolchain.img').open('rb') as old:
        magic,count,_=struct.unpack('<8sII',old.read(16));assert magic==b'AURDEV01' and count<=4096
        for i in range(count):
            old.seek((i+1)*512);metadata=old.read(512)
            name,sector,size,capacity,kind=struct.unpack('<256sQQII232x',metadata);name=name.split(b'\0')[0].decode()
            if not kind:continue
            if kind==2:mkdir(name);continue
            old.seek(sector*512);data=old.read(size);assert len(data)==size
            mode,valid=struct.unpack_from('<II',metadata,280)
            put(name,data,mode if valid else 0o755)
    for folder in ['/usr','/usr/bin','/usr/share','/home','/home/user','/src','/usr/local/bin','/exchange']:mkdir(folder)
    for name in ['include','lib','libexec','x86_64-linux-musl']:symlink('/usr/'+name,'/'+name)
    put('/work/foundations.c',Path('tests/native-foundations.c').read_bytes())
    put('/work/filesystems.c',Path('tests/native-filesystems.c').read_bytes())
    put('/work/Makefile',b'CC = gcc\nCFLAGS = -O2 -static\nall: made-demo\nmade-demo: demo.c\n\t$(CC) $(CFLAGS) demo.c -o made-demo\nclean:\n\trm -f made-demo\n')
    put('/work/rebuild-make.sh',b'#!/bin/bash\nset -e\ncd /work/make-4.4.1\n/usr/bin/make -j1\n./make --version\n/usr/bin/install -m 755 make /usr/local/bin/make\n/usr/local/bin/make --version\necho AURORA_GNU_MAKE_REBUILT\n',0o755)
    if args.gnu:
        with tarfile.open('tools/gnu-bootstrap/gnu-bootstrap.tar.gz') as archive:
            members=archive.getmembers()
            for m in members:
                path='/'+m.name.removeprefix('./').strip('/')
                if path=='/':continue
                if m.isdir():mkdir(path)
                elif m.isfile():put(path,archive.extractfile(m).read(),m.mode)
                elif m.issym():symlink(path,m.linkname)
        symlink('/bin/sh','/usr/bin/bash');symlink('/usr/bin/cc','/bin/gcc')
        for name in ('ar','ranlib','nm','objcopy','objdump','readelf','strip','size','strings','addr2line'):
            symlink('/usr/bin/'+name,'/x86_64-linux-musl/bin/'+name)
        with tarfile.open('tools/gnu-bootstrap/gnu-bootstrap.tar.gz') as outer:
            member=next((m for m in outer if m.name.endswith('/make-configured.tar.gz')),None)
            if member:
                with tarfile.open(fileobj=outer.extractfile(member),mode='r:gz') as configured:
                    for m in configured:
                        path='/work/'+m.name.strip('/')
                        if m.isdir():mkdir(path)
                        elif m.isfile():put(path,configured.extractfile(m).read(),m.mode)
                        elif m.issym():symlink(path,m.linkname)
    for archive in Path('tools/gnu-bootstrap').glob('*.tar.*'):
        if archive.name.startswith(('alpine','gnu-bootstrap')):continue
        put('/src/'+archive.name,archive.read_bytes())
    check(lib.au_close(),'unmount');disk.flush()
    if args.partitioned:
        @callback_type
        def fat_transfer(pointer,sector,count,write):
            try:
                if sector+count>262144:return 5
                disk.seek((1050624+sector)*512)
                if write:disk.write(C.string_at(pointer,count*512))
                else:C.memmove(pointer,disk.read(count*512),count*512)
                return 0
            except Exception:return 5
        check(lib.au_fatformat(fat_transfer),'format FAT32');disk.flush()
temporary.replace(target)
print('Created',target)
