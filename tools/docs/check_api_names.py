#!/usr/bin/env python3
"""Check that the library reference still names things that exist.

The API pages under docs/library/ are hand-curated: signatures are written
for the reader, not extracted from the headers. The cost of that choice is
silent drift -- a renamed or deleted type keeps its documentation page.
This converts drift into a build failure at the granularity that matters:
every API name a page claims must still appear in the headers that page
declares as its sources.

A page declares its sources with an HTML comment near the top:

    <!-- api-headers: include/cpptb/coro_runtime.hpp include/cpptb/test_api.hpp -->

Checked names are the code spans of `###`-level headings (`### Queue<T>`
becomes `Queue`, `### test.start_clock` becomes `start_clock`). Member
tables and prose are not checked; renaming a type without touching its
heading is not a case this needs to catch.

Exit status is the number of missing names.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LIBRARY = ROOT / "docs" / "library"
HEADERS_RE = re.compile(r"<!--\s*api-headers:\s*([^>]*?)\s*-->")
HEADING_RE = re.compile(r"^###\s+(.*)$", re.MULTILINE)
NAME_RE = re.compile(r"`?([A-Za-z_][A-Za-z0-9_]*)")


def heading_name(raw: str) -> str | None:
    """`### test.spawn` -> spawn; `### Queue<T>` -> Queue; prose -> None."""
    text = raw.strip().strip("`")
    if " " in text.replace("...", ""):
        # A heading with spaces is prose ("### Blocking and non-blocking"),
        # not an API name; only single-token headings are checked.
        return None
    token = text.split("<")[0]  # drop template arguments
    token = token.split("(")[0]  # drop call parentheses
    token = token.split(".")[-1]  # test.spawn -> spawn
    token = token.split("::")[-1]
    match = NAME_RE.match(token)
    return match.group(1) if match else None


def main() -> int:
    if not LIBRARY.is_dir():
        return 0
    failures = 0
    for page in sorted(LIBRARY.glob("*.md")):
        text = page.read_text(encoding="utf-8")
        declared = HEADERS_RE.search(text)
        if not declared:
            print(
                f"{page.relative_to(ROOT)}: missing the api-headers comment "
                "naming its source headers",
                file=sys.stderr,
            )
            failures += 1
            continue
        header_paths = [ROOT / part for part in declared.group(1).split()]
        missing_headers = [p for p in header_paths if not p.is_file()]
        if missing_headers:
            for p in missing_headers:
                print(
                    f"{page.relative_to(ROOT)}: declared header does not "
                    f"exist: {p.relative_to(ROOT)}",
                    file=sys.stderr,
                )
            failures += len(missing_headers)
            continue
        corpus = "".join(p.read_text(encoding="utf-8") for p in header_paths)
        for raw in HEADING_RE.findall(text):
            name = heading_name(raw)
            if name is None:
                continue
            if name not in corpus:
                print(
                    f"{page.relative_to(ROOT)}: '{name}' (from heading "
                    f"'{raw.strip()}') not found in declared headers",
                    file=sys.stderr,
                )
                failures += 1
    if failures:
        print(f"check_api_names: {failures} stale reference(s)", file=sys.stderr)
    return failures


if __name__ == "__main__":
    sys.exit(main())
