#!/usr/bin/env python3
"""Resolve a FuseSoC CAPI=2 dependency graph into a Verilator source list.

`dv/uvm/icache` describes itself with `.core` files rather than the `.f` file
lists `dv/uvm/core_ibex` uses, so something has to turn
`lowrisc:dv:ibex_icache_sim:0.1` into an ordered list of sources and include
directories before Verilator can be called.

There were two ways to do that. Driving `fusesoc` is the obvious one, and it is
not taken here: fusesoc is not installed, it is not a standard library module,
and every other tool in this directory is standard library only. It also copies
the whole tree into a work directory, which puts a second copy of the sources
between the build and `deps/` and makes the overlay discipline harder to see.

So this walks the `.core` files in place. What it implements is the part of
CAPI=2 these files use, and nothing else:

  * `filesets`, each with `depend`, `files` and a default `file_type`
  * per-file attributes, `{is_include_file: true}` and `{file_type: ...}`
  * `targets`, each naming filesets, with `tool_x ? (fileset)` conditions
  * `virtual:`, the mechanism by which `lowrisc:prim_generic:ram_1p` provides
    `lowrisc:prim:ram_1p`

Anything outside that -- generators, parameters, `filters`, `use_vpi`, tool
sections -- is not implemented and is not reached from this graph. A dependency
that cannot be resolved is an error rather than something to skip.

The virtual resolution picks the `prim_generic` provider, which is what
`common_sim_cfg.hjson` asks for with
`sv_flist_gen_flags: ["--mapping=lowrisc:prim_generic:all:0.1"]`.

Standard library only. That includes the YAML: `parse` below reads the subset
of block YAML these files are written in, in the same spirit as the testlist
reader in ports/core_ibex_uvm/run_directed.py.
"""

from __future__ import annotations

import re
from pathlib import Path


class CoreError(Exception):
    """Something about the .core graph does not hold."""


# ---------------------------------------------------------------------------
# The YAML subset
# ---------------------------------------------------------------------------

# A flow mapping, `{is_include_file: true}`. These files never nest one.
FLOW_MAP = re.compile(r"^\{(.*)\}$")


def _scalar(text: str):
    """A YAML plain or quoted scalar, with the three values that matter typed."""
    text = text.strip()
    if len(text) >= 2 and text[0] == text[-1] and text[0] in "\"'":
        return text[1:-1]
    if text in ("true", "True"):
        return True
    if text in ("false", "False"):
        return False
    if text in ("null", "~", ""):
        return None
    return text


def _strip_comment(line: str) -> str:
    """Drop a `#` comment, leaving one inside quotes alone."""
    quote = None
    for index, char in enumerate(line):
        if quote:
            if char == quote:
                quote = None
        elif char in "\"'":
            quote = char
        elif char == "#" and (index == 0 or line[index - 1] in " \t"):
            return line[:index]
    return line


def _value(text: str, anchors: dict):
    """The right-hand side of a `key:` or the body of a `- ` item."""
    text = text.strip()
    if text.startswith("*"):
        name = text[1:].strip()
        if name not in anchors:
            raise CoreError(f"alias *{name} with no anchor")
        return anchors[name]
    match = FLOW_MAP.match(text)
    if match:
        out = {}
        body = match.group(1).strip()
        if body:
            for part in body.split(","):
                key, separator, item = part.partition(":")
                if not separator:
                    raise CoreError(f"flow mapping entry with no key: {part!r}")
                out[key.strip()] = _scalar(item)
        return out
    return _scalar(text)


def parse(text: str):
    """Read the block-YAML subset the .core files use.

    Supported: nested block mappings, block sequences, flow mappings as a
    value, anchors on a mapping key (`default: &default_target`) and aliases
    to them. Not supported, and not present in this graph: block scalars,
    merge keys, nested flow collections, multi-document files.
    """
    lines = []
    for number, raw in enumerate(text.splitlines(), 1):
        body = _strip_comment(raw).rstrip()
        if not body.strip():
            continue
        lines.append((number, len(body) - len(body.lstrip()), body.strip()))

    anchors: dict = {}

    def block(start: int, indent: int):
        """Parse one collection at `indent`, returning (value, next index)."""
        index = start
        if lines[index][2].startswith("- "):
            out_list = []
            while index < len(lines):
                number, column, body = lines[index]
                if column < indent or not body.startswith("- "):
                    break
                if column > indent:
                    raise CoreError(f"line {number}: unexpected indent")
                index += 1
                rest = body[2:].strip()
                key, separator, tail = _split_key(rest)
                if separator and not tail:
                    # `- key:` opening a nested block belonging to the item.
                    inner, index = block(index, indent + 2)
                    out_list.append({key: inner})
                elif separator:
                    out_list.append({key: _value(tail, anchors)})
                else:
                    out_list.append(_value(rest, anchors))
            return out_list, index

        out_map: dict = {}
        while index < len(lines):
            number, column, body = lines[index]
            if column < indent:
                break
            if column > indent:
                raise CoreError(f"line {number}: unexpected indent")
            if body.startswith("- "):
                break
            key, separator, tail = _split_key(body)
            if not separator:
                raise CoreError(f"line {number}: no key in {body!r}")
            index += 1
            anchor = None
            if tail.startswith("&"):
                anchor, _, tail = tail.partition(" ")
                anchor = anchor[1:]
                tail = tail.strip()
            if tail:
                out_map[key] = _value(tail, anchors)
            elif index < len(lines) and lines[index][1] > column:
                out_map[key], index = block(index, lines[index][1])
            else:
                out_map[key] = None
            if anchor:
                anchors[anchor] = out_map[key]
        return out_map, index

    if not lines:
        return {}
    value, index = block(0, lines[0][1])
    if index != len(lines):
        raise CoreError(f"line {lines[index][0]}: trailing content")
    return value


def _split_key(body: str) -> tuple[str, str, str]:
    """`key: value` split that leaves a `:` inside a VLNV or a quote alone."""
    quote = None
    depth = 0
    for index, char in enumerate(body):
        if quote:
            if char == quote:
                quote = None
        elif char in "\"'":
            quote = char
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
        elif char == ":" and depth == 0:
            # A VLNV such as `lowrisc:dv:dv_lib` is a scalar, not a mapping:
            # the key separator is a colon followed by a space or end of line.
            if index + 1 < len(body) and body[index + 1] not in " \t":
                continue
            return body[:index].strip(), ":", body[index + 1:].strip()
    return body.strip(), "", ""


# ---------------------------------------------------------------------------
# The core index
# ---------------------------------------------------------------------------

NAME_LINE = re.compile(r'^name\s*:\s*"?([^"\s]+)"?\s*$', re.M)
VIRTUAL_BLOCK = re.compile(r'^virtual\s*:\s*\n((?:\s*-\s*\S+\s*\n)+)', re.M)


def vlnv(name: str) -> str:
    """vendor:library:core, dropping the version.

    Dependencies are written both with and without a version in this tree --
    `lowrisc:prim:assert` against `lowrisc:prim:assert:0.1` -- and fusesoc
    treats the versionless form as "any version". There is exactly one version
    of everything here, so dropping it is enough.
    """
    return ":".join(name.split(":")[:3])


class Index:
    """Every .core file under a root, by VLNV, with the virtual providers."""

    def __init__(self, root: Path):
        self.root = root
        self.by_name: dict[str, Path] = {}
        self.providers: dict[str, list[str]] = {}
        self._parsed: dict[Path, dict] = {}

        # Scanned with a regex rather than parsed, so that a .core file this
        # graph never reaches cannot fail the build by using YAML the reader
        # above does not implement. Files in the graph are parsed in full.
        for path in sorted(root.rglob("*.core")):
            text = path.read_text(encoding="utf-8")
            if not text.startswith("CAPI=2"):
                continue
            match = NAME_LINE.search(text)
            if not match:
                continue
            key = vlnv(match.group(1))
            if key in self.by_name:
                raise CoreError(f"two cores named {key}: "
                                f"{self.by_name[key]} and {path}")
            self.by_name[key] = path
            block = VIRTUAL_BLOCK.search(text)
            if block:
                for line in block.group(1).splitlines():
                    provided = vlnv(line.strip().lstrip("-").strip())
                    self.providers.setdefault(provided, []).append(match.group(1))
        if not self.by_name:
            raise CoreError(f"no .core files under {root}")

    def data(self, name: str) -> dict:
        path = self.path(name)
        if path not in self._parsed:
            text = path.read_text(encoding="utf-8")
            self._parsed[path] = parse(text.split("\n", 1)[1])
        return self._parsed[path]

    def path(self, name: str) -> Path:
        key = vlnv(name)
        if key not in self.by_name:
            raise CoreError(f"no core provides {key}")
        return self.by_name[key]

    def resolve(self, dependency: str) -> str:
        """A `depend:` entry to the core that satisfies it.

        A dependency may carry a version constraint (`>=0.1`), which is
        dropped: this tree vendors one version of everything.
        """
        key = vlnv(re.split(r"[ <>=^~]", dependency.strip())[0])
        if key in self.by_name:
            return key
        if key in self.providers:
            options = self.providers[key]
            generic = [o for o in options if o.startswith("lowrisc:prim_generic:")]
            if generic:
                return vlnv(generic[0])
            if len(options) > 1:
                raise CoreError(f"{key} has several providers and none is "
                                f"prim_generic: {options}")
            return vlnv(options[0])
        raise CoreError(f"no core provides {key}")


# ---------------------------------------------------------------------------
# Walking a target
# ---------------------------------------------------------------------------

class SourceFile:
    __slots__ = ("path", "is_include", "file_type", "core")

    def __init__(self, path: Path, is_include: bool, file_type: str, core: str):
        self.path = path
        self.is_include = is_include
        self.file_type = file_type
        self.core = core

    def __repr__(self) -> str:
        return f"<{self.core} {self.path.name} {self.file_type}>"


CONDITION = re.compile(r"^(?P<flags>[^?]+)\?\s*\((?P<names>[^)]*)\)$")


def _filesets(core: str, target: dict, flags: set[str]) -> list[str]:
    """The fileset names a target selects, honouring `tool_x ? (...)`."""
    out = []
    for entry in target.get("filesets") or []:
        if not isinstance(entry, str):
            raise CoreError(f"{core}: unexpected fileset entry {entry!r}")
        match = CONDITION.match(entry.strip())
        if not match:
            out.append(entry.strip())
            continue
        wanted = match.group("flags").split()
        # fusesoc's expression language also has `!flag`; nothing in this
        # graph uses one, so a negation is an error rather than a guess.
        if any(flag.startswith("!") for flag in wanted):
            raise CoreError(f"{core}: negated flag in {entry!r} is not "
                            f"implemented")
        if all(flag in flags for flag in wanted):
            out.extend(match.group("names").split())
    return out


def walk(index: Index, top: str, target_name: str,
         flags: set[str] | None = None) -> list[SourceFile]:
    """Every file a target pulls in, dependencies before dependants.

    The order is fusesoc's: a post-order walk of the dependency graph, with a
    core's own files emitted after everything it depends on. Verilator does
    not need a package declared before it is imported, but keeping upstream's
    order means a difference in the built file list is a difference in the
    graph rather than in this walker.
    """
    flags = flags if flags is not None else {"tool_verilator"}
    files: list[SourceFile] = []
    done: set[str] = set()

    def visit(name: str, target: str) -> None:
        key = vlnv(name)
        if key in done:
            return
        done.add(key)
        data = index.data(key)
        directory = index.path(key).parent
        targets = data.get("targets") or {}
        chosen = targets.get(target)
        if chosen is None:
            chosen = targets.get("default")
        if chosen is None:
            if targets:
                raise CoreError(f"{key}: no `{target}` or `default` target")
            chosen = {}

        own: list[SourceFile] = []
        for fileset_name in _filesets(key, chosen, flags):
            fileset = (data.get("filesets") or {}).get(fileset_name)
            if fileset is None:
                raise CoreError(f"{key}: target names fileset "
                                f"{fileset_name!r}, which it does not define")
            for dependency in fileset.get("depend") or []:
                visit(index.resolve(dependency), "default")
            default_type = fileset.get("file_type")
            for entry in fileset.get("files") or []:
                if isinstance(entry, dict):
                    if len(entry) != 1:
                        raise CoreError(f"{key}: unexpected file entry {entry!r}")
                    (filename, attributes), = entry.items()
                    attributes = attributes or {}
                else:
                    filename, attributes = entry, {}
                path = (directory / filename).resolve()
                if not path.is_file():
                    raise CoreError(f"{key}: {filename} does not exist "
                                    f"({path})")
                own.append(SourceFile(
                    path,
                    bool(attributes.get("is_include_file", False)),
                    attributes.get("file_type") or default_type or "",
                    key))
        files.extend(own)

    visit(top, target_name)
    return files


def toplevel(index: Index, top: str, target_name: str) -> str:
    data = index.data(top)
    target = (data.get("targets") or {}).get(target_name) or {}
    name = target.get("toplevel")
    if not name:
        raise CoreError(f"{top}: target {target_name} names no toplevel")
    return name


def sources_and_incdirs(files: list[SourceFile]) -> tuple[list[Path], list[Path],
                                                          list[Path]]:
    """(compiled sources, include directories, Verilator control files).

    A file marked `is_include_file` is not compiled; its directory becomes an
    include path. Everything else with a SystemVerilog file type is compiled.
    `vlt` files are Verilator control files, which upstream's own verilator
    target selects through `tool_verilator ? (files_verilator_waiver)`.
    Anything else -- the `user` file type on two Python scripts in
    `lowrisc:tool:check_tool_requirements` -- is dropped.
    """
    sources: list[Path] = []
    incdirs: list[Path] = []
    control: list[Path] = []
    sv_types = {"systemVerilogSource", "verilogSource"}
    for entry in files:
        if entry.is_include:
            if entry.path.parent not in incdirs:
                incdirs.append(entry.path.parent)
            continue
        if entry.file_type == "vlt":
            control.append(entry.path)
        elif entry.file_type in sv_types:
            sources.append(entry.path)
    return sources, incdirs, control
