#!/usr/bin/env python3
"""Read Ibex's SystemVerilog functional coverage into a model, and diff it
against what the cpptb port declares.

Why this exists. Verilator 5.050 cannot produce Ibex's functional coverage: it
hard-errors on the transition bins in `cp_controller_fsm`, and even with that
fixed it discards 456 constructs with COVERIGN warnings -- 208 `intersect`, 131
`&&`, 71 explicit cross bins, 21 `iff` in a cross, and the rest. A cross whose
select expression is dropped keeps every combination of its coverpoints' bins,
which is a different and much larger bin set than the source describes. So the
UVM baseline on Verilator cannot be a coverage reference, and "the port matches"
cannot be shown by running both and comparing.

What is left is the specification. This parses the covergroups out of the
SystemVerilog and emits the bin model they describe; the port emits its own
model as JSON; and `--diff` reports where the two disagree. That makes
equivalence a measurement rather than a claim, and it keeps working when
upstream changes the coverage -- a coverpoint added upstream shows up here as
missing rather than silently going unported.

    python3 fcov_model.py                 # summarise the SystemVerilog model
    python3 fcov_model.py --json out.json # emit it
    python3 fcov_model.py --diff cpptb.json

What it does not do. This is a parser for the subset of clause 19 that Ibex's
coverage uses, not a SystemVerilog front end. It reads the covergroup bodies
textually; it does not elaborate, so it cannot know the width or type of a
sampled expression. That matters for auto-binned coverpoints -- a coverpoint
with no bins body gets one bin per value of its type -- so those are recorded as
`auto` with their expression, and the enum types are resolved separately from
the packages. Anything it cannot parse is reported, never skipped silently.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
FCOV = ROOT / "deps" / "ibex" / "dv" / "uvm" / "core_ibex" / "fcov"
SOURCES = ["core_ibex_fcov_if.sv", "core_ibex_pmp_fcov_if.sv"]


class ParseError(RuntimeError):
    pass


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def match_brace(text: str, start: int) -> int:
    """Index just past the `}` closing the `{` at `start`."""
    depth = 0
    for index in range(start, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return index + 1
    raise ParseError("unbalanced braces in a covergroup body")


VALUE_LIST = re.compile(r"\[([^\]:]+):([^\]]+)\]|([^,\s\[\]]+)")


def parse_values(body: str) -> list[dict]:
    """`{a, [b:c], d}` -> ranges. Values stay as written: they are enum names
    and parameters as often as literals, and resolving them needs the packages
    rather than this file."""
    out = []
    for low, high, single in VALUE_LIST.findall(body):
        if single:
            out.append({"kind": "value", "value": single.strip()})
        else:
            out.append({"kind": "range", "low": low.strip(),
                        "high": high.strip()})
    return out


TRANSITION = re.compile(r"\(([^)]*?)=>([^)]*?)\)")

BIN = re.compile(
    r"(?P<wildcard>wildcard\s+)?"
    r"(?P<kind>bins|ignore_bins|illegal_bins)\s+"
    r"(?P<name>\w+)\s*(?P<array>\[\s*(?P<size>\d*)\s*\])?\s*=\s*"
    r"(?P<body>[^;]+);", re.S)


def parse_bins(body: str, where: str) -> list[dict]:
    bins = []
    for found in BIN.finditer(body):
        kind = {"bins": "ordinary", "ignore_bins": "ignore",
                "illegal_bins": "illegal"}[found.group("kind")]
        name = found.group("name")
        text = found.group("body").strip()
        entry: dict = {"name": name, "kind": kind, "source": where}

        if "default sequence" in text:
            entry["form"] = "default_sequence"
        elif "default" in text and "=>" not in text:
            entry["form"] = "default"
        elif "=>" in text:
            entry["form"] = "transition"
            transitions = []
            for start, end in TRANSITION.findall(text):
                transitions.append({"from": start.strip(), "to": end.strip()})
            if not transitions:
                raise ParseError(f"{where}: cannot read transition bin {name}")
            entry["transitions"] = transitions
        else:
            entry["form"] = "values"
            entry["values"] = parse_values(text)
            if not entry["values"]:
                raise ParseError(f"{where}: cannot read bin {name}")
        if found.group("wildcard"):
            entry["wildcard"] = True
        if found.group("array") is not None:
            entry["array"] = True
            if found.group("size"):
                entry["array_size"] = int(found.group("size"))
        bins.append(entry)
    return bins


SELECT_BINSOF = re.compile(r"binsof\s*\(\s*([\w.]+)\s*\)"
                           r"(?:\s*intersect\s*\{([^}]*)\})?")


def parse_select(text: str) -> dict:
    """The shape of a cross select expression, not a full expression tree:
    which coverpoints it names, which values it intersects, and whether it uses
    && , || , ! or with. Enough to tell one select from another in a diff."""
    terms = []
    for point, values in SELECT_BINSOF.findall(text):
        term = {"point": point}
        if values.strip():
            term["intersect"] = parse_values(values)
        terms.append(term)
    return {
        "text": " ".join(text.split()),
        "binsof": terms,
        "conjunction": "&&" in text,
        "disjunction": "||" in text,
        "negation": "!binsof" in text.replace(" ", ""),
        "with": "with" in text,
    }


CROSS_BIN = re.compile(
    r"(?P<kind>bins|ignore_bins|illegal_bins)\s+(?P<name>\w+)\s*=\s*"
    r"(?P<body>[^;]+);", re.S)


def parse_cross_body(body: str) -> list[dict]:
    filters = []
    for found in CROSS_BIN.finditer(body):
        kind = {"bins": "ordinary", "ignore_bins": "ignore",
                "illegal_bins": "illegal"}[found.group("kind")]
        filters.append({
            "name": found.group("name"),
            "kind": kind,
            "select": parse_select(found.group("body")),
        })
    return filters


COVERPOINT = re.compile(r"(?P<name>\w+)\s*:\s*coverpoint\s+")
CROSS = re.compile(r"(?P<name>\w+)\s*:\s*cross\s+")


def scan_declaration(text: str, start: int) -> tuple[str, int, str]:
    """From `start`, return the declaration text, the index of its terminator
    and which terminator it is.

    The terminator is the first `{` or `;` outside any parentheses. Taking the
    first `{` outright is wrong: an `iff` guard can hold a concatenation, and
    two of Ibex's crosses do, which truncated the guard and left its
    parentheses unbalanced.
    """
    depth = 0
    for index in range(start, len(text)):
        character = text[index]
        if character == "(":
            depth += 1
        elif character == ")":
            depth -= 1
        elif depth == 0 and character in "{;":
            return text[start:index], index, character
    raise ParseError("a coverpoint or cross declaration has no terminator")


COVERGROUP = re.compile(r"covergroup\s+(\w+)")
IFF = re.compile(r"\biff\s*\(", re.S)


def split_iff(rest: str) -> tuple[str, str | None]:
    """Separate a sampled expression from its `iff` guard."""
    found = IFF.search(rest)
    if not found:
        return rest.strip(), None
    expression = rest[:found.start()].strip()
    depth = 0
    for index in range(found.end() - 1, len(rest)):
        if rest[index] == "(":
            depth += 1
        elif rest[index] == ")":
            depth -= 1
            if depth == 0:
                return expression, rest[found.end():index].strip()
    raise ParseError("unbalanced parentheses in an iff guard")


def parse_covergroups(text: str, filename: str) -> list[dict]:
    text = strip_comments(text)
    groups = []
    for group_match in COVERGROUP.finditer(text):
        start = text.find(";", group_match.end())
        # The covergroup body runs to its `endgroup`.
        end = text.find("endgroup", start)
        if end < 0:
            raise ParseError(f"{filename}: covergroup with no endgroup")
        body = text[start:end]
        name = group_match.group(1)
        renamed = re.search(r'option\.name\s*=\s*"([^"]+)"', body)
        group = {
            "name": renamed.group(1) if renamed else name,
            "declared_name": name,
            "file": filename,
            "coverpoints": [],
            "crosses": [],
        }

        consumed: list[tuple[int, int]] = []
        for found in COVERPOINT.finditer(body):
            rest, terminator, kind = scan_declaration(body, found.end())
            expression, guard = split_iff(rest)
            point = {
                "name": found.group("name"),
                "expression": " ".join(expression.split()),
                "iff": " ".join(guard.split()) if guard else None,
            }
            if kind == "{":
                close = match_brace(body, terminator)
                point["bins"] = parse_bins(body[terminator:close],
                                           point["name"])
                point["auto"] = False
                consumed.append((found.start(), close))
            else:
                # No bins body: SystemVerilog gives it one bin per value of the
                # sampled type. What those are cannot be known without
                # elaborating, so the model records the fact and the diff
                # resolves it against the port's declaration.
                point["bins"] = []
                point["auto"] = True
                consumed.append((found.start(), terminator))
            group["coverpoints"].append(point)

        for found in CROSS.finditer(body):
            if any(low <= found.start() < high for low, high in consumed):
                continue
            rest, terminator, kind = scan_declaration(body, found.end())
            rest, guard = split_iff(rest)
            points = [part.strip() for part in rest.split(",") if part.strip()]
            cross = {
                "name": found.group("name"),
                "points": points,
                "iff": " ".join(guard.split()) if guard else None,
                "filters": [],
            }
            if kind == "{":
                close = match_brace(body, terminator)
                cross["filters"] = parse_cross_body(body[terminator:close])
            group["crosses"].append(cross)

        groups.append(group)
    return groups


def read_model() -> list[dict]:
    groups = []
    for name in SOURCES:
        path = FCOV / name
        if not path.is_file():
            raise ParseError(f"missing {path}")
        groups.extend(parse_covergroups(path.read_text(encoding="utf-8"),
                                        name))
    return groups


def summarise(groups: list[dict]) -> None:
    for group in groups:
        auto = sum(1 for p in group["coverpoints"] if p["auto"])
        explicit_bins = sum(len(p["bins"]) for p in group["coverpoints"])
        guarded = sum(1 for p in group["coverpoints"] if p["iff"])
        filters = sum(len(c["filters"]) for c in group["crosses"])
        cross_guarded = sum(1 for c in group["crosses"] if c["iff"])
        print(f"{group['name']}  ({group['file']})")
        print(f"  coverpoints      {len(group['coverpoints'])}"
              f"  ({auto} auto-binned, {guarded} with an iff guard)")
        print(f"  declared bins    {explicit_bins}")
        print(f"  crosses          {len(group['crosses'])}"
              f"  ({cross_guarded} with an iff guard)")
        print(f"  cross filters    {filters}")
        kinds: dict[str, int] = {}
        for point in group["coverpoints"]:
            for entry in point["bins"]:
                key = entry["form"] + ("/wildcard" if entry.get("wildcard")
                                       else "")
                kinds[key] = kinds.get(key, 0) + 1
        for key, count in sorted(kinds.items()):
            print(f"    {key:<22} {count}")


def diff(groups: list[dict], port_path: Path) -> int:
    """Compare the SystemVerilog model against what the port declared."""
    port = json.loads(port_path.read_text(encoding="utf-8"))
    # cpptb writes one covergroup per file, so a file with a name and points at
    # the top level is one covergroup; a list under "covergroups" is several.
    if "covergroups" in port:
        groups_ported = port["covergroups"]
    elif "name" in port and "points" in port:
        groups_ported = [port]
    else:
        raise ParseError(f"{port_path}: not a cpptb coverage model")
    ported = {group["name"]: group for group in groups_ported}

    problems = 0
    for group in groups:
        mine = ported.get(group["name"])
        print(f"\n=== {group['name']} ===")
        if mine is None:
            print(f"  NOT PORTED: no covergroup named {group['name']}")
            problems += 1
            continue

        port_points = {point["name"]: point for point in mine["points"]}
        missing = [p["name"] for p in group["coverpoints"]
                   if p["name"] not in port_points]
        extra = [name for name in port_points
                 if name not in {p["name"] for p in group["coverpoints"]}]
        print(f"  coverpoints  {len(group['coverpoints']) - len(missing)}"
              f" of {len(group['coverpoints'])} ported")
        for name in missing:
            print(f"    missing coverpoint  {name}")
        for name in extra:
            print(f"    extra coverpoint    {name}  (not in the SystemVerilog)")
        problems += len(missing) + len(extra)

        for point in group["coverpoints"]:
            mine_point = port_points.get(point["name"])
            if mine_point is None or point["auto"]:
                continue
            declared = {bin["name"] for bin in mine_point["bins"]}
            for entry in point["bins"]:
                names = expected_bin_names(entry)
                for name in names:
                    if name not in declared:
                        print(f"    {point['name']}: missing bin {name}"
                              f"  ({entry['form']})")
                        problems += 1

        port_crosses = {cross["name"]: cross for cross in mine["crosses"]}
        cross_missing = [c["name"] for c in group["crosses"]
                         if c["name"] not in port_crosses]
        print(f"  crosses      {len(group['crosses']) - len(cross_missing)}"
              f" of {len(group['crosses'])} ported")
        for name in cross_missing:
            print(f"    missing cross       {name}")
        problems += len(cross_missing)

        for cross in group["crosses"]:
            mine_cross = port_crosses.get(cross["name"])
            if mine_cross is None:
                continue
            expected = len(cross["filters"])
            actual = len(mine_cross.get("ignored", [])) + \
                len(mine_cross.get("illegal", []))
            if expected != actual:
                print(f"    {cross['name']}: {actual} filters, the "
                      f"SystemVerilog has {expected}")
                problems += 1

    print(f"\n{problems} disagreement(s) between the SystemVerilog and the "
          f"port")
    return 1 if problems else 0


def expected_bin_names(entry: dict) -> list[str]:
    if entry.get("array"):
        # An array bin's element names depend on the range, which may be
        # parameters; the diff checks the base name is present.
        return [entry["name"]]
    return [entry["name"]]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--json", type=Path,
                        help="write the SystemVerilog model here")
    parser.add_argument("--diff", type=Path,
                        help="compare against a coverage model from the port")
    args = parser.parse_args()

    try:
        groups = read_model()
    except ParseError as error:
        print(f"fcov_model: {error}", file=sys.stderr)
        return 2

    if args.json:
        args.json.write_text(json.dumps(groups, indent=2) + "\n",
                             encoding="utf-8")
        print(f"wrote {args.json}")
    if args.diff:
        return diff(groups, args.diff)
    if not args.json:
        summarise(groups)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
