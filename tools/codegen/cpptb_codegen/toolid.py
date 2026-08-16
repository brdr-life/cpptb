"""Toolchain self-identity for build fingerprints.

Project cache keys include the toolchain's own source bytes, so that editing
the generator invalidates caches. A frozen single-binary release has no
source files on disk; the executable's own content hash carries the same
information -- any rebuild of the binary changes it, and nothing else does.
"""

from __future__ import annotations

import functools
import hashlib
import sys
from pathlib import Path


def frozen() -> bool:
    return bool(getattr(sys, "frozen", False))


@functools.cache
def frozen_identity() -> bytes:
    digest = hashlib.sha256()
    digest.update(Path(sys.executable).read_bytes())
    return digest.hexdigest().encode()
