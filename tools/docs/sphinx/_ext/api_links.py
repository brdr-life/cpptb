"""Auto-link inline code to the library reference.

Every ``###``-level heading in ``docs/library/*.md`` is an API name with a
stable anchor (the same convention ``tools/docs/check_api_names.py``
enforces). This extension walks each resolved page and turns inline code
that names one of those APIs into a link to its reference section, so
``co_await ReadOnly{}`` in any concept page becomes clickable without
hand-maintaining links in the prose.

Matching is deliberately conservative. A code span is linked only when,
after stripping call syntax, it reduces to exactly one known name:

    ReadOnly              -> ReadOnly
    co_await ReadOnly{};  -> ReadOnly
    clock_cycles(clk, n)  -> clock_cycles
    test.spawn(task())    -> spawn
    dut.count.get()       -> get
    Queue<Packet>         -> Queue

Names claimed by more than one library page are dropped as ambiguous, a
span already inside a link is left alone, and spans on the defining page
itself are not self-linked. Fenced code blocks are untouched -- only
inline code participates.
"""

from __future__ import annotations


import re
from pathlib import Path

from docutils import nodes

HEADING_RE = re.compile(r"^###\s+(.*)$", re.MULTILINE)
NAME_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")

# `text` -> ("library/<page>", "<anchor>") built once per build.
_TARGETS: dict[str, tuple[str, str]] = {}


def _heading_name(raw: str) -> str | None:
    text = raw.strip().strip("`")
    if " " in text:
        return None
    token = text.split("<")[0].split("(")[0]
    return token if NAME_RE.match(token) else None


def _anchor(name: str) -> str:
    # docutils section id for a single-token heading: lowercased, with
    # underscores kept (docutils allows them after its id normalization of
    # word characters; CPPTB macro names become e.g. cpptb-register-test).
    slug = name.lower().replace("_", "-")
    return slug


def _build_targets(app) -> None:
    _TARGETS.clear()
    library = Path(app.srcdir) / "library"
    if not library.is_dir():
        return
    claimed: dict[str, str] = {}
    ambiguous: set[str] = set()
    for page in sorted(library.glob("*.md")):
        docname = f"library/{page.stem}"
        for raw in HEADING_RE.findall(page.read_text(encoding="utf-8")):
            name = _heading_name(raw)
            if name is None:
                continue
            if name in claimed and claimed[name] != docname:
                ambiguous.add(name)
                continue
            claimed[name] = docname
            _TARGETS[name] = (docname, _anchor(name))
    for name in ambiguous:
        _TARGETS.pop(name, None)


def _resolve(span: str) -> str | None:
    """Reduce a code span to a candidate API name, or None."""
    text = span.strip().rstrip(";").strip()
    if text.startswith("co_await "):
        text = text[len("co_await "):].strip()
    # Strip one trailing call/brace/template group, if balanced at the end.
    for open_char, close_char in (("(", ")"), ("{", "}"), ("<", ">")):
        if text.endswith(close_char) and open_char in text:
            text = text[: text.index(open_char)]
            break
    text = text.strip()
    if "." in text:
        text = text.rsplit(".", 1)[-1]
    if "::" in text:
        text = text.rsplit("::", 1)[-1]
    return text if NAME_RE.match(text) else None


def _foreign_column(literal: nodes.literal) -> bool:
    """True when the span sits in a table column whose header names another
    framework (a "cocotb" column in a comparison table): those cells describe
    the other tool's API, so linking them to our reference would mislead."""
    entry = None
    node = literal
    while node is not None:
        if isinstance(node, nodes.entry):
            entry = node
        if isinstance(node, nodes.table):
            break
        node = node.parent
    if entry is None or node is None:
        return False
    row = entry.parent
    column = row.index(entry)
    for head in node.findall(nodes.thead):
        for header_row in head.findall(nodes.row):
            cells = list(header_row.findall(nodes.entry))
            if column < len(cells) and cells[column].astext().strip().lower() == "cocotb":
                return True
    return False


def _link_literals(app, doctree, docname: str) -> None:
    if not _TARGETS or app.builder.format != "html":
        return
    for literal in list(doctree.findall(nodes.literal)):
        parent = literal.parent
        if parent is None:
            continue
        # Already a link, a heading, or block-level code: leave it alone.
        if isinstance(parent, (nodes.reference, nodes.title)):
            continue
        if _foreign_column(literal):
            continue
        name = _resolve(literal.astext())
        if name is None:
            continue
        target = _TARGETS.get(name)
        if target is None or target[0] == docname:
            continue
        uri = app.builder.get_relative_uri(docname, target[0]) + "#" + target[1]
        reference = nodes.reference(
            "", "", literal.deepcopy(), refuri=uri, internal=True
        )
        parent.replace(literal, reference)


def setup(app):
    app.connect("builder-inited", _build_targets)
    app.connect("doctree-resolved", _link_literals)
    # Sphinx does not treat an extension edit as a reason to re-render
    # cached doctrees, and a default computed here is not compared against
    # the cached environment. The Makefile passes this file's content hash
    # as a -D override instead -- overrides ARE compared, so an edit here
    # invalidates every cached page.
    app.add_config_value("api_links_source_hash", "", "env")
    return {"version": "1", "parallel_read_safe": True, "parallel_write_safe": True}
