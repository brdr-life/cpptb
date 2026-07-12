"""Compatibility frontend for Verilator's JSON-only elaboration output."""

from __future__ import annotations

import json
import re
import subprocess
import tempfile
from pathlib import Path
from typing import Any

from cpptb.codegen.design_ir import (
    CodegenError,
    DesignIR,
    PackedEnumType,
    PackedEnumValue,
    PackedField,
    PackedIntegralType,
    PackedRange,
    PackedStructType,
    PackedType,
    PackedUnionType,
    Port,
    UnpackedRange,
)
from cpptb.codegen.frontends import frontend_options


def _frontend_args(manifest: dict[str, Any]) -> list[str]:
    return list(frontend_options(manifest, "verilator_json").get("args", []))


def _define_args(manifest: dict[str, Any]) -> list[str]:
    defines = manifest.get("defines", {})
    if isinstance(defines, list):
        return [f"-D{value}" for value in defines]
    if isinstance(defines, dict):
        return [
            f"-D{name}" if value is None else f"-D{name}={value}"
            for name, value in defines.items()
        ]
    raise CodegenError("manifest defines must be an object or list")


def verilator_ast(manifest: dict[str, Any], base_dir: Path) -> dict[str, Any]:
    sources = [str((base_dir / source).resolve()) for source in manifest["sources"]]
    include_dirs = [
        f"-I{(base_dir / include_dir).resolve()}"
        for include_dir in manifest.get("include_dirs", [])
    ]
    parameters = [
        f"-G{name}={value}" for name, value in manifest.get("parameters", {}).items()
    ]

    with tempfile.TemporaryDirectory(prefix="cpptb-codegen-") as temp_dir:
        ast_path = Path(temp_dir) / "dut.tree.json"
        meta_path = Path(temp_dir) / "dut.tree.meta.json"
        command = [
            manifest.get("verilator", "verilator"),
            "--json-only",
            "--top-module",
            manifest["module"],
            "--json-only-output",
            str(ast_path),
            "--json-only-meta-output",
            str(meta_path),
            *manifest.get("verilator_args", []),
            *_frontend_args(manifest),
            *include_dirs,
            *_define_args(manifest),
            *parameters,
            *sources,
        ]
        completed = subprocess.run(
            command,
            cwd=base_dir,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if completed.returncode != 0:
            raise CodegenError(
                "Verilator could not elaborate the DUT:\n" + completed.stdout.rstrip()
            )
        try:
            return json.loads(ast_path.read_text())
        except (OSError, json.JSONDecodeError) as error:
            raise CodegenError(f"cannot read Verilator JSON AST: {error}") from error


def walk_objects(value: Any):
    if isinstance(value, dict):
        yield value
        for child in value.values():
            yield from walk_objects(child)
    elif isinstance(value, list):
        for child in value:
            yield from walk_objects(child)


def parse_width(dtype: dict[str, Any], port_name: str) -> int:
    if dtype.get("type") != "BASICDTYPE":
        raise CodegenError(
            f"port {port_name!r} uses unsupported dtype {dtype.get('type')!r}; "
            "only scalar and packed integral ports are supported"
        )
    bit_range = dtype.get("range")
    if not bit_range:
        return 1
    match = re.fullmatch(r"(-?\d+):(-?\d+)", bit_range)
    if not match:
        raise CodegenError(
            f"port {port_name!r} has unsupported elaborated range {bit_range!r}"
        )
    left, right = (int(value) for value in match.groups())
    return abs(left - right) + 1


def parse_range(text: str, port_name: str) -> UnpackedRange:
    match = re.fullmatch(
        r"(?:\[(-?\d+):(-?\d+)\]|(-?\d+):(-?\d+))", text
    )
    if not match:
        raise CodegenError(
            f"port {port_name!r} has unsupported elaborated range {text!r}"
        )
    bracketed_left, bracketed_right, bare_left, bare_right = match.groups()
    left = int(bracketed_left if bracketed_left is not None else bare_left)
    right = int(bracketed_right if bracketed_right is not None else bare_right)
    return UnpackedRange(left, right)


def _dtype_reference(
    dtype: dict[str, Any], dtypes: dict[str, dict[str, Any]], name: str
) -> dict[str, Any]:
    for key in ("refDTypep", "subDTypep", "dtypep"):
        address = dtype.get(key)
        if not address or address == dtype.get("addr"):
            continue
        referenced = dtypes.get(address)
        if referenced is not None:
            return referenced
    raise CodegenError(
        f"cannot resolve the referenced dtype for {name!r} "
        f"from {dtype.get('type')!r}"
    )


def _dereference_dtype(
    dtype: dict[str, Any], dtypes: dict[str, dict[str, Any]], name: str
) -> tuple[dict[str, Any], str | None]:
    declared_name: str | None = None
    seen: set[str] = set()
    current = dtype
    while current.get("type") in {"REFDTYPE", "TYPEDEFDTYPE"}:
        address = current.get("addr")
        if address in seen:
            raise CodegenError(f"dtype reference cycle while resolving {name!r}")
        if address:
            seen.add(address)
        declared_name = declared_name or current.get("name") or None
        current = _dtype_reference(current, dtypes, name)
    return current, declared_name


def _constant_integer(text: str, name: str) -> int:
    normalized = text.replace("_", "").lower()
    match = re.fullmatch(
        r"(?P<negative>-)?(?P<width>\d+)'(?P<signed>s)?"
        r"(?P<base>[bodh])(?P<digits>-?[0-9a-fxz?]+)",
        normalized,
    )
    if not match:
        try:
            return int(normalized, 0)
        except ValueError as error:
            raise CodegenError(
                f"enum value {name!r} has unsupported constant {text!r}"
            ) from error

    digits = match.group("digits")
    if any(character in digits for character in "xz?"):
        raise CodegenError(f"enum value {name!r} contains X or Z bits")
    radix = {"b": 2, "o": 8, "d": 10, "h": 16}[match.group("base")]
    value = int(digits, radix)
    width = int(match.group("width"))
    if match.group("signed") and value >= 0 and value & (1 << (width - 1)):
        value -= 1 << width
    if match.group("negative"):
        value = -value
    return value


def _enum_values(
    dtype: dict[str, Any], name: str, base: PackedIntegralType
) -> tuple[PackedEnumValue, ...]:
    values: list[PackedEnumValue] = []
    for item in dtype.get("itemsp", []):
        value_nodes = item.get("valuep", [])
        if len(value_nodes) != 1 or not value_nodes[0].get("name"):
            raise CodegenError(
                f"enum value {item.get('name', '<unnamed>')!r} in {name!r} "
                "has no single elaborated constant"
            )
        item_name = item.get("name", "")
        value = _constant_integer(value_nodes[0]["name"], item_name)
        if base.signed and value >= 0 and value & (1 << (base.width - 1)):
            value -= 1 << base.width
        values.append(PackedEnumValue(item_name, value))
    return tuple(values)


def packed_type(
    dtype: dict[str, Any], dtypes: dict[str, dict[str, Any]], name: str
) -> PackedType:
    dtype, alias_name = _dereference_dtype(dtype, dtypes, name)
    kind = dtype.get("type")
    declared_name = alias_name or dtype.get("name") or None

    if kind == "BASICDTYPE":
        bit_range = dtype.get("range")
        parsed_range = parse_range(bit_range, name) if bit_range else None
        ranges = (
            (PackedRange(parsed_range.left, parsed_range.right),)
            if parsed_range is not None
            else ()
        )
        return PackedIntegralType(
            parse_width(dtype, name),
            bool(dtype.get("signed", False)),
            dtype.get("name") != "bit",
            ranges,
            alias_name,
        )

    if kind == "PACKARRAYDTYPE":
        declared_range = dtype.get("declRange") or dtype.get("range")
        if not declared_range:
            raise CodegenError(f"packed array {name!r} has no fixed range")
        parsed_range = parse_range(declared_range, name)
        inner = packed_type(_dtype_reference(dtype, dtypes, name), dtypes, name)
        if not isinstance(inner, PackedIntegralType):
            raise CodegenError(
                f"packed array {name!r} has unsupported aggregate element type"
            )
        return PackedIntegralType(
            parsed_range.size * inner.width,
            bool(dtype.get("signed", inner.signed)),
            inner.four_state,
            (PackedRange(parsed_range.left, parsed_range.right), *inner.ranges),
            declared_name,
        )

    if kind == "ENUMDTYPE":
        base = packed_type(_dtype_reference(dtype, dtypes, name), dtypes, name)
        if not isinstance(base, PackedIntegralType):
            raise CodegenError(f"packed enum {name!r} has a non-integral base type")
        return PackedEnumType(base, _enum_values(dtype, name, base), declared_name)

    if kind in {"STRUCTDTYPE", "UNIONDTYPE"}:
        members: list[tuple[str, PackedType]] = []
        for member in dtype.get("membersp", []):
            member_name = member.get("name", "")
            members.append(
                (
                    member_name,
                    packed_type(
                        _dtype_reference(member, dtypes, member_name),
                        dtypes,
                        member_name,
                    ),
                )
            )
        if not members:
            raise CodegenError(f"packed aggregate {name!r} has no members")
        if kind == "UNIONDTYPE":
            width = max(member_type.width for _, member_type in members)
            fields = tuple(
                PackedField(member_name, member_type, 0)
                for member_name, member_type in members
            )
            return PackedUnionType(
                width,
                bool(dtype.get("signed", False)),
                any(member_type.four_state for _, member_type in members),
                fields,
                declared_name,
            )

        width = sum(member_type.width for _, member_type in members)
        offset = width
        fields: list[PackedField] = []
        for member_name, member_type in members:
            offset -= member_type.width
            fields.append(PackedField(member_name, member_type, offset))
        return PackedStructType(
            width,
            bool(dtype.get("signed", False)),
            any(member_type.four_state for _, member_type in members),
            tuple(fields),
            declared_name,
        )

    raise CodegenError(
        f"port {name!r} uses unsupported dtype {kind!r}; expected a packed "
        "integral, enum, struct, or union"
    )


def resolve_port_dtype(
    dtype: dict[str, Any], dtypes: dict[str, dict[str, Any]], port_name: str
) -> tuple[dict[str, Any], tuple[UnpackedRange, ...]]:
    dimensions: list[UnpackedRange] = []
    element_type, _ = _dereference_dtype(dtype, dtypes, port_name)
    while element_type.get("type") == "UNPACKARRAYDTYPE":
        declared_range = element_type.get("declRange")
        if not declared_range:
            raise CodegenError(
                f"port {port_name!r} has an unpacked array without a fixed range"
            )
        dimensions.append(parse_range(declared_range, port_name))
        element_type = _dtype_reference(element_type, dtypes, port_name)
        element_type, _ = _dereference_dtype(element_type, dtypes, port_name)
    return element_type, tuple(dimensions)


def discover_ports(ast: dict[str, Any], module_name: str) -> list[Port]:
    objects = list(walk_objects(ast))
    module = next(
        (
            item
            for item in objects
            if item.get("type") == "MODULE" and item.get("name") == module_name
        ),
        None,
    )
    if module is None:
        raise CodegenError(f"module {module_name!r} was not found in Verilator AST")

    dtypes = {
        item["addr"]: item
        for item in objects
        if item.get("type", "").endswith("DTYPE") and "addr" in item
    }
    ports: list[Port] = []
    for statement in module.get("stmtsp", []):
        if statement.get("type") != "VAR" or statement.get("varType") != "PORT":
            continue
        name = statement["name"]
        direction = statement.get("direction", "").lower()
        if direction not in {"input", "output", "inout"}:
            raise CodegenError(
                f"port {name!r} has unsupported direction {direction!r}"
            )
        dtype = dtypes.get(statement.get("dtypep"))
        if dtype is None:
            raise CodegenError(f"cannot resolve the dtype for port {name!r}")
        element_type, unpacked = resolve_port_dtype(dtype, dtypes, name)
        data_type = packed_type(element_type, dtypes, name)
        ports.append(
            Port(
                name=name,
                direction=direction,
                width=data_type.width,
                type_kind=(
                    "packed_union"
                    if isinstance(data_type, PackedUnionType)
                    else "integral"
                ),
                signed=data_type.signed,
                four_state=data_type.four_state,
                unpacked=unpacked,
                packed_type=data_type,
            )
        )

    if not ports:
        raise CodegenError(f"module {module_name!r} has no discoverable ports")
    return ports


class VerilatorJsonFrontend:
    name = "verilator_json"

    def elaborate(self, manifest: dict[str, Any], base_dir: Path) -> DesignIR:
        if manifest.get("internals"):
            raise CodegenError(
                "the verilator_json frontend cannot resolve configured internals; "
                "use the Slang frontend"
            )
        ast = verilator_ast(manifest, base_dir)
        ports = discover_ports(ast, manifest["module"])
        return DesignIR(manifest["module"], tuple(ports))
