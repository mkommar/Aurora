"""Test kernel interpreter handling through execve, without shell fallback."""
from pathlib import Path
from aurora_vm import run

if __name__ == '__main__':
    run('tests/exec-scripts.sh', 'build/exec-script-tests',
        files={'/work/exec-scripts.c': Path('tests/exec-scripts.c').read_bytes()})
    assert 'AURORA_EXEC_SCRIPTS_PASS' in Path('build/exec-script-tests/serial.log').read_text()
