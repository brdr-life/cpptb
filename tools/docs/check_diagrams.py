#!/usr/bin/env python3
"""Check box alignment in ASCII diagrams inside docs fenced code blocks.

A lopsided box -- a border row and its content rows disagreeing about where
the box's edges are -- survives a normal read because the eye forgives one
column. This walks every fenced code block under docs/ that uses box-drawing
characters and enforces the rectangle invariant for each box it finds:

  * from every `┌`, the top border runs to a `┐` on the same row (text is
    allowed inside the border, so titled frames pass);
  * every row below carries a vertical edge character in both border columns
    until a row closes the box with `└` and `┘` in those exact columns.

Trees drawn with `├──`/`└──` and flow spines contain no `┌`, so they are not
constrained. Exit status is the number of misaligned boxes.
"""

from __future__ import annotations

import sys
from pathlib import Path

DOCS = Path(__file__).resolve().parents[2] / "docs"
BOX_CHARS = set("┌┐└┘├┤┬┴┼─│")
VERTICAL_OK = set("│├┤┼")


def fenced_blocks(text: str):
    """Yield (start_line, list_of_lines) for each fenced code block."""
    lines = text.splitlines()
    block: list[str] | None = None
    start = 0
    for number, line in enumerate(lines, 1):
        if line.lstrip().startswith("```"):
            if block is None:
                block, start = [], number + 1
            else:
                yield start, block
                block = None
        elif block is not None:
            block.append(line)


def char_at(row: str, column: int) -> str:
    return row[column] if column < len(row) else " "


def check_box(block: list[str], top: int, left: int) -> str | None:
    """Validate the box whose `┌` sits at (top, left); return an error or None."""
    row = block[top]
    right = None
    for column in range(left + 1, len(row)):
        if row[column] == "┐":
            right = column
            break
        if row[column] == "┌":
            return None  # nested/adjacent top; that box is checked on its own
    if right is None:
        return f"top border opened at column {left} never closes with ┐"
    for below in range(top + 1, len(block)):
        line = block[below]
        left_char, right_char = char_at(line, left), char_at(line, right)
        if left_char == "└" or right_char == "┘":
            if left_char != "└" or right_char != "┘":
                return (
                    f"bottom border at line offset {below} closes at only one "
                    f"edge (columns {left} and {right})"
                )
            return None
        if left_char not in VERTICAL_OK or right_char not in VERTICAL_OK:
            return (
                f"row at line offset {below} misses the box edges: expected "
                f"vertical edges in columns {left} and {right}, found "
                f"{left_char!r} and {right_char!r}"
            )
    return f"box opened at column {left} has no bottom border"


def main() -> int:
    failures = 0
    for path in sorted(DOCS.rglob("*.md")):
        text = path.read_text(encoding="utf-8")
        for start, block in fenced_blocks(text):
            if not any(set(line) & BOX_CHARS for line in block):
                continue
            for row_index, line in enumerate(block):
                for column, char in enumerate(line):
                    if char != "┌":
                        continue
                    error = check_box(block, row_index, column)
                    if error:
                        failures += 1
                        print(
                            f"{path.relative_to(DOCS.parent)}:"
                            f"{start + row_index}: {error}",
                            file=sys.stderr,
                        )
    if failures:
        print(f"check_diagrams: {failures} misaligned box(es)", file=sys.stderr)
    return failures


if __name__ == "__main__":
    sys.exit(main())
