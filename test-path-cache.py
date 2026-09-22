"""Exceed the production cache capacity while retaining an open descriptor."""
from pathlib import Path
from aurora_vm import run

if __name__ == '__main__':
    files={f'/work/cache-fixture/d{i//100:03d}/f{i%100:03d}':b'' for i in range(20000)}
    files['/work/path-cache.c']=Path('tests/path-cache.c').read_bytes()
    run('tests/path-cache.sh','build/path-cache-tests',files=files,cpus=4,port=4457)
