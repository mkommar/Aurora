"""Host fixtures for native transaction state and bounded evidence handling."""
import json
from pathlib import Path
import tempfile

from transaction_observability import (format_status, read_checkpoint,
                                       resume_stage, safe_state_dir, tail,
                                       write_checkpoint)

with tempfile.TemporaryDirectory(prefix='aurora-observability-') as temporary:
    root = Path(temporary) / 'state'
    state = safe_state_dir(root, 'diffutils')
    write_checkpoint(state, 'compile', 'running', 'jobs=4')
    assert read_checkpoint(state)['stage'] == 'compile'
    assert resume_stage(state) == 'compile'
    assert 'stage=compile' in format_status(state, state / 'build.log')
    (state / 'build.log').write_text('x' * 5000, encoding='utf-8')
    assert len(tail(state / 'build.log', 600)) == 600
    write_checkpoint(state, 'compile', 'timeout', 'stale checkpoint')
    assert read_checkpoint(state)['status'] == 'timeout'
    write_checkpoint(state, 'lifecycle', 'passed', 'complete')
    assert resume_stage(state) is None
    assert len((state / 'checkpoints.jsonl').read_text().splitlines()) == 3
    try:
        safe_state_dir(root, '../escape')
    except ValueError:
        pass
    else:
        raise AssertionError('accepted unsafe state path')
    try:
        write_checkpoint(state, 'not-a-stage', 'running')
    except ValueError:
        pass
    else:
        raise AssertionError('accepted unknown stage')
    assert json.loads((state / 'current.json').read_text())['detail'] == 'complete'

print('PASS transaction observability: checkpoints, path safety, bounded tails and status')
