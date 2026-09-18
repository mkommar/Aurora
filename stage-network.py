"""Stage networking onto a copy. The source development disk is never written."""
import argparse,hashlib,json,shutil
from pathlib import Path
from image_access import put_ext2_files
from network_artifacts import files
p=argparse.ArgumentParser();p.add_argument('--source',default='build/development.img');p.add_argument('--output',default='build/development-network.img');a=p.parse_args()
source=Path(a.source).resolve();output=Path(a.output).resolve()
if source==output or output.exists():raise SystemExit('Choose a new output path distinct from the source disk.')
payload=files();digest=hashlib.sha256(source.read_bytes()).hexdigest()
shutil.copyfile(source,output);put_ext2_files(output,payload)
output.with_suffix('.network-stage.json').write_text(json.dumps({'source':str(source),'source_sha256':digest,'output':str(output),'files':{p:hashlib.sha256(d).hexdigest() for p,d in payload.items()}},indent=2))
print('Staged',output,'; boot it and run chmod 755 /bin/curl, then sync.')
