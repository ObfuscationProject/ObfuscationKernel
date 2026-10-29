"""Source sets for hosted runners that build from a retained source snapshot.

A runner names the translation units and inputs it means to retain; the quoted
include closure of those files is added here, so a header that gains an include
does not silently break the snapshot. Whole-tree snapshots copy only the source
trees, never the checkout root (build output, caches and untracked links live
there).
"""
from pathlib import Path
import re

SNAPSHOT_TREES = ('include', 'src', 'tests')
_INCLUDE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.M)
_SCANNED = {'.c', '.cc', '.cpp', '.h', '.hpp', '.inc', '.S'}


def include_closure(root, names, include_dirs=('include',)):
    """Return the repository-relative names plus every file they reach through quoted includes.

    A quoted include resolves against the including file's directory and then
    each include directory, as the compiler searches it. Includes that resolve
    outside the repository or not at all (generated or system headers) are left
    to the compiler.
    """
    root = Path(root).resolve()
    bases = [root / directory for directory in include_dirs]
    pending = [root / name for name in names]
    found = set()
    while pending:
        path = pending.pop()
        if path in found:
            continue
        if not path.is_file() or path.is_symlink():
            raise ValueError('explicit regular input required: ' + str(path))
        found.add(path)
        if path.suffix not in _SCANNED:
            continue
        for name in _INCLUDE.findall(path.read_text(errors='replace')):
            for base in (path.parent, *bases):
                candidate = (base / name).resolve()
                if candidate.is_file() and candidate.is_relative_to(root):
                    pending.append(candidate)
                    break
    return sorted(path.relative_to(root).as_posix() for path in found)


def snapshot_members(root):
    """Yield the regular files of the source trees in a stable order."""
    root = Path(root).resolve()
    for tree in SNAPSHOT_TREES:
        for path in sorted((root / tree).rglob('*')):
            if path.is_file() and not path.is_symlink():
                yield path
