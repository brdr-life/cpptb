#!/usr/bin/env python3
"""Fetch pinned upstream projects into deps/.

Sources are cloned at an exact commit and never committed to this repository,
so deps/ is disposable and the tree stays small. Fetching is shallow: a bare
init followed by a fetch of the single pinned commit, because `git clone
--branch` does not accept a commit.

    python3 fetch.py --list
    python3 fetch.py ibex
    python3 fetch.py --all

Standard library only, so this runs before any dependency is installed.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCES = HERE / "sources.toml"
DEPS = HERE / "deps"


class FetchError(RuntimeError):
    """A source could not be fetched or did not match its pin."""


def load_sources() -> dict[str, dict]:
    if not SOURCES.is_file():
        raise FetchError(f"missing {SOURCES}")
    data = tomllib.loads(SOURCES.read_text(encoding="utf-8"))
    for name, entry in data.items():
        for required in ("repo", "commit"):
            if required not in entry:
                raise FetchError(f"[{name}] is missing '{required}'")
    return data


def _run(command: list[str], cwd: Path) -> None:
    completed = subprocess.run(
        command, cwd=cwd, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
    )
    if completed.returncode != 0:
        raise FetchError(
            f"{' '.join(command)} failed with {completed.returncode}:\n"
            f"{completed.stdout.strip()}"
        )


def fetch(name: str, entry: dict, *, force: bool) -> Path:
    target = DEPS / name
    commit = entry["commit"]

    if target.exists():
        if not force and _head(target) == commit:
            print(f"{name}: already at {commit[:12]}")
            return target
        shutil.rmtree(target)

    target.mkdir(parents=True)
    print(f"{name}: fetching {commit[:12]} from {entry['repo']}")
    _run(["git", "init", "--quiet"], target)
    _run(["git", "remote", "add", "origin", entry["repo"]], target)
    # Shallow fetch of one commit; GitHub allows fetching a SHA directly.
    _run(["git", "fetch", "--quiet", "--depth", "1", "origin", commit], target)
    _run(["git", "checkout", "--quiet", "FETCH_HEAD"], target)

    actual = _head(target)
    if actual != commit:
        raise FetchError(
            f"{name}: checked out {actual} but sources.toml pins {commit}"
        )
    print(f"{name}: at {commit[:12]} in {target.relative_to(HERE)}")
    return target


def _head(target: Path) -> str | None:
    """Return the checked-out commit, or None if there is not one.

    Covers the not-yet-fetched case: subprocess raises rather than returning a
    status when cwd does not exist.
    """
    if not (target / ".git").is_dir():
        return None
    try:
        completed = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=target, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=False,
        )
    except OSError:
        return None
    return completed.stdout.strip() if completed.returncode == 0 else None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("names", nargs="*", help="sources to fetch")
    parser.add_argument("--all", action="store_true", help="fetch every source")
    parser.add_argument("--list", action="store_true", help="list sources")
    parser.add_argument("--force", action="store_true",
                        help="refetch even when already at the pinned commit")
    args = parser.parse_args(argv)

    try:
        sources = load_sources()
    except FetchError as error:
        print(f"fetch: {error}", file=sys.stderr)
        return 1

    if args.list or not (args.names or args.all):
        if not sources:
            print("  no sources pinned in sources.toml")
            return 0
        width = max(len(name) for name in sources)
        for name, entry in sources.items():
            state = _head(DEPS / name)
            status = ("fetched" if state == entry["commit"]
                      else "stale" if state else "absent")
            print(f"  {name:<{width}}  {entry['commit'][:12]}  {status:<7}"
                  f"  {entry.get('license', '?')}")
        return 0

    selected = list(sources) if args.all else args.names
    unknown = [name for name in selected if name not in sources]
    if unknown:
        print(f"fetch: unknown source(s): {', '.join(unknown)}", file=sys.stderr)
        return 1

    for name in selected:
        try:
            fetch(name, sources[name], force=args.force)
        except FetchError as error:
            print(f"fetch: {error}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
