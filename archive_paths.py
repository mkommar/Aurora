"""Safe, platform-neutral archive member path comparisons."""
import posixpath
import re


_DRIVE_PATH = re.compile(r'^[A-Za-z]:/')


def normalize_archive_path(name):
    """Return a safe comparison form without changing the archive member."""
    if not isinstance(name, str) or '\x00' in name:
        raise ValueError('Invalid archive path')
    path = name.replace('\\', '/')
    if path.startswith('/') or _DRIVE_PATH.match(path):
        raise ValueError('Absolute archive path')
    if '..' in path.split('/'):
        raise ValueError('Unsafe archive path')
    path = posixpath.normpath(path)
    if path in ('', '.') or path.startswith('/'):
        raise ValueError('Unsafe archive path')
    return path


def find_archive_member(members, requested):
    """Find a member by safe normalized name, returning its original object."""
    wanted = normalize_archive_path(requested)
    match = None
    for member in members:
        if normalize_archive_path(member.name) != wanted:
            continue
        if match is not None:
            raise ValueError(f'Duplicate archive path: {requested}')
        match = member
    return match
