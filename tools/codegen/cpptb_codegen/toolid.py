"""Toolchain self-identity for build fingerprints.

Project cache keys include the toolchain's own source bytes, so that editing
the generator invalidates caches. A frozen single-binary release has no
source files on disk; the executable's own content hash carries the same
information -- any rebuild of the binary changes it, and nothing else does.
"""

from __future__ import annotations

import functools
import hashlib
import shutil
import sys
from pathlib import Path


def executable_path() -> Path:
    """The invoked tool binary, robust to both freezers.

    PyInstaller keeps sys.executable pointing at the real binary. Nuitka
    onefile points it at a synthetic `python` inside the extraction
    directory that does not exist on disk; the invoked binary is argv[0].
    """
    exe = Path(sys.executable)
    if exe.exists():
        return exe
    candidate = Path(sys.argv[0])
    if not candidate.exists():
        located = shutil.which(sys.argv[0])
        if located:
            candidate = Path(located)
    return candidate.resolve()


def frozen() -> bool:
    # PyInstaller sets sys.frozen; Nuitka defines __compiled__ in every
    # module it compiles, this one included.
    return bool(getattr(sys, "frozen", False)) or "__compiled__" in globals()


@functools.cache
def frozen_identity() -> bytes:
    digest = hashlib.sha256()
    with executable_path().open("rb") as exe:
        for chunk in iter(lambda: exe.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest().encode()
