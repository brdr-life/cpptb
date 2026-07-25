#!/usr/bin/env python3
"""Convert a flat binary into a VMEM file for $readmemh.

Upstream Ibex uses srecord for this:

    srec_cat prog.bin -binary -offset 0x0000 -byte-swap 4 -o prog.vmem -vmem

srecord is a system package, and needing root to produce firmware for a
simulation is a poor trade for what the conversion actually is. The memories
these ports load are 32 bits wide and read with $readmemh, so a VMEM is one
hex word per location, which is what -byte-swap 4 arranges for.

    python3 bin2vmem.py prog.bin prog.vmem

Standard library only, matching fetch.py.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

WORD = 4


def convert(data: bytes, *, words_per_line: int = 1, offset: int = 0) -> str:
    if len(data) % WORD:
        # Zero-fill: a trailing partial word is a short final section, not an
        # error, and the memory is word addressed regardless.
        data = data + bytes(WORD - len(data) % WORD)

    lines = [f"@{offset // WORD:08X}"]
    words = [
        int.from_bytes(data[index:index + WORD], "little")
        for index in range(0, len(data), WORD)
    ]
    for start in range(0, len(words), words_per_line):
        chunk = words[start:start + words_per_line]
        lines.append(" ".join(f"{word:08X}" for word in chunk))
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("binary", type=Path, help="flat binary to convert")
    parser.add_argument("vmem", type=Path, help="VMEM file to write")
    parser.add_argument("--offset", type=lambda v: int(v, 0), default=0,
                        help="byte address the binary loads at (default 0)")
    parser.add_argument("--words-per-line", type=int, default=1,
                        help="words per output line (default 1)")
    args = parser.parse_args(argv)

    if not args.binary.is_file():
        print(f"bin2vmem: no such file: {args.binary}", file=sys.stderr)
        return 1
    if args.offset % WORD:
        print(f"bin2vmem: offset {args.offset:#x} is not word aligned",
              file=sys.stderr)
        return 1

    data = args.binary.read_bytes()
    args.vmem.write_text(
        convert(data, words_per_line=args.words_per_line, offset=args.offset),
        encoding="utf-8",
    )
    print(f"{args.vmem}: {len(data)} bytes, "
          f"{(len(data) + WORD - 1) // WORD} words")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
