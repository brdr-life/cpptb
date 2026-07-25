#!/usr/bin/env python3
"""Fetch pinned upstream sources into deps/.

Two kinds of source, both pinned exactly and neither committed to this
repository, so deps/ is disposable and the tree stays small:

- ``git``: cloned at one commit. Fetching is shallow, using init plus a fetch
  of the single commit, because ``git clone --branch`` does not accept one.
- ``archive``: downloaded and checked against a SHA-256 digest before it is
  unpacked, so a changed or substituted asset fails rather than installing.

    python3 fetch.py --list
    python3 fetch.py ibex
    python3 fetch.py --all

Standard library only, so this runs before any dependency is installed.
"""

from __future__ import annotations

import argparse
import hashlib
import platform
import shutil
import subprocess
import sys
import tarfile
import tomllib
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCES = HERE / "sources.toml"
DEPS = HERE / "deps"
CACHE = DEPS / ".cache"
STAMP = ".fetch-pin"


class FetchError(RuntimeError):
    """A source could not be fetched or did not match its pin."""


def load_sources() -> dict[str, dict]:
    if not SOURCES.is_file():
        raise FetchError(f"missing {SOURCES}")
    data = tomllib.loads(SOURCES.read_text(encoding="utf-8"))
    for name, entry in data.items():
        kind = _kind(entry)
        required = ("repo", "commit") if kind == "git" else ("url", "sha256")
        for key in required:
            if key not in entry:
                raise FetchError(f"[{name}] is a {kind} source missing '{key}'")
    return data


def _kind(entry: dict) -> str:
    return entry.get("kind", "git" if "repo" in entry else "archive")


def pin_of(entry: dict) -> str:
    """The value a fetched tree must match."""
    return entry["commit"] if _kind(entry) == "git" else entry["sha256"]


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


def current_pin(name: str, entry: dict) -> str | None:
    """What is checked out, or None when nothing is."""
    target = DEPS / name
    if _kind(entry) == "git":
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
    stamp = target / STAMP
    return stamp.read_text(encoding="utf-8").strip() if stamp.is_file() else None


def _check_platform(name: str, entry: dict) -> None:
    wanted = entry.get("platform")
    if not wanted:
        return
    actual = f"{platform.system()}-{platform.machine()}"
    if wanted != actual:
        raise FetchError(
            f"{name} is pinned for {wanted} but this host is {actual}; pin an "
            "asset for this platform in sources.toml"
        )


def _fetch_git(name: str, entry: dict, target: Path) -> None:
    commit = entry["commit"]
    print(f"{name}: fetching {commit[:12]} from {entry['repo']}")
    _run(["git", "init", "--quiet"], target)
    _run(["git", "remote", "add", "origin", entry["repo"]], target)
    _run(["git", "fetch", "--quiet", "--depth", "1", "origin", commit], target)
    _run(["git", "checkout", "--quiet", "FETCH_HEAD"], target)


def _download(url: str, destination: Path) -> str:
    """Download to destination, returning the SHA-256 of what arrived."""
    digest = hashlib.sha256()
    destination.parent.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(url) as response, destination.open("wb") as out:
        while chunk := response.read(1 << 20):
            digest.update(chunk)
            out.write(chunk)
    return digest.hexdigest()


def _fetch_archive(name: str, entry: dict, target: Path) -> None:
    expected = entry["sha256"]
    archive = CACHE / f"{name}-{expected[:12]}{_suffix(entry['url'])}"

    if archive.is_file() and _sha256(archive) == expected:
        print(f"{name}: reusing verified download")
    else:
        print(f"{name}: downloading {entry['url']}")
        actual = _download(entry["url"], archive)
        if actual != expected:
            archive.unlink(missing_ok=True)
            raise FetchError(
                f"{name}: downloaded archive has sha256 {actual} but "
                f"sources.toml pins {expected}"
            )
        print(f"{name}: sha256 verified")

    strip = int(entry.get("strip_components", 0))
    print(f"{name}: unpacking")
    with tarfile.open(archive) as tar:
        members = []
        for member in tar.getmembers():
            parts = Path(member.name).parts[strip:]
            if not parts:
                continue
            member.name = str(Path(*parts))
            # A hard link records its target as an archive path, so stripping
            # only the member name leaves it pointing at a name no longer in
            # the archive. Symlinks are relative and must be left alone.
            if member.islnk():
                link_parts = Path(member.linkname).parts[strip:]
                if link_parts:
                    member.linkname = str(Path(*link_parts))
            members.append(member)
        # filter="data" refuses absolute paths, traversal, and special files.
        tar.extractall(target, members=members, filter="data")
    (target / STAMP).write_text(expected + "\n", encoding="utf-8")


def _suffix(url: str) -> str:
    name = url.rsplit("/", 1)[-1]
    for candidate in (".tar.xz", ".tar.gz", ".tar.bz2", ".tgz", ".tar"):
        if name.endswith(candidate):
            return candidate
    return ".tar"


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while chunk := handle.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def fetch(name: str, entry: dict, *, force: bool) -> Path:
    _check_platform(name, entry)
    target = DEPS / name
    pin = pin_of(entry)

    if target.exists():
        if not force and current_pin(name, entry) == pin:
            print(f"{name}: already at {pin[:12]}")
            return target
        shutil.rmtree(target)

    target.mkdir(parents=True)
    if _kind(entry) == "git":
        _fetch_git(name, entry, target)
    else:
        _fetch_archive(name, entry, target)

    actual = current_pin(name, entry)
    if actual != pin:
        raise FetchError(
            f"{name}: fetched {actual} but sources.toml pins {pin}"
        )
    print(f"{name}: at {pin[:12]} in {target.relative_to(HERE)}")
    return target


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("names", nargs="*", help="sources to fetch")
    parser.add_argument("--all", action="store_true", help="fetch every source")
    parser.add_argument("--list", action="store_true", help="list sources")
    parser.add_argument("--force", action="store_true",
                        help="refetch even when already at the pinned version")
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
            pin = pin_of(entry)
            state = current_pin(name, entry)
            status = ("fetched" if state == pin
                      else "stale" if state else "absent")
            print(f"  {name:<{width}}  {_kind(entry):<7}  {pin[:12]}  "
                  f"{status:<7}  {entry.get('license', '?')}")
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
