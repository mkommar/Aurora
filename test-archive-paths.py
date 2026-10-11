"""Regression coverage for safe archive member lookup and staging paths."""
import io
import tarfile

from archive_paths import find_archive_member, normalize_archive_path
from image_access import EXECUTABLE_PATHS


def member(name):
    info = tarfile.TarInfo(name)
    info.size = 1
    return info


assert normalize_archive_path('./bin/busybox') == 'bin/busybox'
assert normalize_archive_path(r'.\bin\busybox') == 'bin/busybox'
assert find_archive_member([member('./bin/busybox')], 'bin/busybox').name == './bin/busybox'
assert find_archive_member([member('bin/tool')], './bin/tool').name == 'bin/tool'

for unsafe in ('../bin/busybox', '/bin/busybox', r'..\bin\busybox', r'C:\bin\busybox'):
    try:
        normalize_archive_path(unsafe)
    except ValueError:
        pass
    else:
        raise AssertionError(f'unsafe archive path accepted: {unsafe}')

assert '/usr/bin/xz' in EXECUTABLE_PATHS
print('PASS archive member normalization, safe lookup, traversal rejection and executable staging')
