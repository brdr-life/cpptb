#!/usr/bin/env python3
"""Generate a typed C++ DUT binding and a batched SystemVerilog DPI wrapper."""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import OrderedDict
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Any

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from cpptb.codegen.design_ir import (
    CodegenError,
    DesignIR,
    Internal,
    PackedEnumType,
    PackedField,
    PackedIntegralType,
    PackedStructType,
    PackedType,
    PackedUnionType,
    Port,
)
from cpptb.codegen.frontends import elaborate_design, frontend_name
from cpptb.codegen.frontends.verilator_json import (
    discover_ports,
    parse_width,
    verilator_ast,
    walk_objects,
)


CLOCK_SOURCES = frozenset({"generated", "testbench", "dut"})
TIME_UNIT_FEMTOSECONDS = {
    "fs": 1,
    "ps": 1_000,
    "ns": 1_000_000,
    "us": 1_000_000_000,
    "ms": 1_000_000_000_000,
    "s": 1_000_000_000_000_000,
}


def time_literal_femtoseconds(value: str, label: str) -> int:
    match = re.fullmatch(r"\s*(\d+)\s*(fs|ps|ns|us|ms|s)\s*", value)
    if match is None:
        raise CodegenError(
            f"{label} must be an integer SystemVerilog time literal, got {value!r}"
        )
    femtoseconds = int(match.group(1)) * TIME_UNIT_FEMTOSECONDS[match.group(2)]
    if femtoseconds == 0:
        raise CodegenError(f"{label} cannot be zero")
    return femtoseconds


@dataclass
class TreeNode:
    path: tuple[str, ...]
    children: OrderedDict[str, TreeNode | Port | Internal] = field(
        default_factory=OrderedDict
    )


def load_manifest(path: Path) -> dict[str, Any]:
    try:
        manifest = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise CodegenError(f"cannot read manifest {path}: {error}") from error

    if manifest.get("schema_version") != 1:
        raise CodegenError("manifest schema_version must be 1")
    for key in ("module", "top_module", "namespace", "root_type", "sources"):
        if key not in manifest:
            raise CodegenError(f"manifest is missing required key {key!r}")
    if "clock" not in manifest and "clocks" not in manifest:
        raise CodegenError("manifest must define clock or clocks")
    return manifest


def clock_configs(manifest: dict[str, Any]) -> list[dict[str, Any]]:
    clocks = manifest.get("clocks")
    if clocks is None:
        clocks = [manifest["clock"]]
    if not isinstance(clocks, list):
        raise CodegenError("manifest clocks must be a list")
    if not all(isinstance(clock, dict) for clock in clocks):
        raise CodegenError("each clock configuration must be an object")
    return clocks


def clock_source(clock: dict[str, Any]) -> str:
    return clock.get("source", "generated")


def generated_clock_names(manifest: dict[str, Any]) -> set[str]:
    return {
        clock["port"]
        for clock in clock_configs(manifest)
        if clock_source(clock) == "generated"
    }


def validate_clock_ports(manifest: dict[str, Any], ports: list[Port]) -> None:
    clocks = clock_configs(manifest)
    ports_by_name = {port.name: port for port in ports}
    seen_clocks: set[str] = set()
    primary_count = 0

    for clock in clocks:
        clock_name = clock.get("port")
        if not isinstance(clock_name, str) or not clock_name:
            raise CodegenError("each clock must name a port")
        if clock_name in seen_clocks:
            raise CodegenError(f"clock port {clock_name!r} is configured more than once")
        seen_clocks.add(clock_name)

        source = clock_source(clock)
        if source not in CLOCK_SOURCES:
            choices = ", ".join(sorted(CLOCK_SOURCES))
            raise CodegenError(
                f"clock port {clock_name!r} has invalid source {source!r}; "
                f"expected one of {choices}"
            )

        port = ports_by_name.get(clock_name)
        if port is None or port.width != 1:
            raise CodegenError(
                f"clock port {clock_name!r} must name one single-bit DUT port"
            )
        expected_direction = "output" if source == "dut" else "input"
        if port.direction != expected_direction:
            raise CodegenError(
                f"{source} clock port {clock_name!r} must be a DUT "
                f"{expected_direction}"
            )

        if source != "generated" and any(
            key in clock for key in ("half_period", "phase")
        ):
            raise CodegenError(
                f"{source} clock port {clock_name!r} cannot define "
                "half_period or phase"
            )
        if clock.get("primary", False):
            primary_count += 1

    if primary_count > 1:
        raise CodegenError("only one clock may be marked primary")


def validate_identifier(value: str, label: str) -> None:
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", value):
        raise CodegenError(f"{label} {value!r} is not a valid C++ identifier")


def path_for_port(port_name: str, manifest: dict[str, Any]) -> tuple[str, ...]:
    aliases = manifest.get("aliases", {})
    if port_name in aliases:
        path = tuple(aliases[port_name].split("."))
    else:
        path = (port_name,)
        for rule in manifest.get("path_rules", []):
            prefix = rule["prefix"]
            if not port_name.startswith(prefix):
                continue
            suffix = port_name[len(prefix) :]
            path = tuple(rule["path"].split("."))
            for group in rule.get("groups", []):
                if suffix in group["members"]:
                    path += tuple(group["path"].split("."))
                    break
            path += (suffix,)
            break

    if not path or any(not item for item in path):
        raise CodegenError(f"port {port_name!r} maps to an invalid empty C++ path")
    for item in path:
        validate_identifier(item, f"C++ path component for port {port_name!r}")
    return path


def map_ports(ports: list[Port], manifest: dict[str, Any]) -> list[Port]:
    mapped = [
        replace(port, cpp_path=path_for_port(port.name, manifest))
        for port in ports
    ]
    paths: dict[tuple[str, ...], str] = {}
    for port in mapped:
        previous = paths.setdefault(port.cpp_path, port.name)
        if previous != port.name:
            raise CodegenError(
                f"ports {previous!r} and {port.name!r} map to the same C++ path "
                f"{'.'.join(port.cpp_path)!r}"
            )
    return mapped


def packed_union_path(
    data_type: PackedType, path: tuple[str, ...]
) -> tuple[str, ...] | None:
    if isinstance(data_type, PackedUnionType):
        return path
    if isinstance(data_type, PackedStructType):
        for field_info in data_type.fields:
            found = packed_union_path(
                field_info.data_type, (*path, field_info.name)
            )
            if found is not None:
                return found
    return None


def validate_transport_ports(ports: list[Port]) -> None:
    for port in ports:
        if port.type_kind != "integral" or port.width < 1:
            raise CodegenError(
                f"port {port.name!r} uses unsupported elaborated type "
                f"{port.type_kind!r}; only scalar and packed integral ports "
                "are supported"
            )
        if port.packed_type is not None:
            union_path = packed_union_path(port.packed_type, (port.name,))
            if union_path is not None:
                raise CodegenError(
                    f"port {port.name!r} contains unsupported packed union at "
                    f"{'.'.join(union_path)!r}"
                )
        if port.direction not in {"input", "output"}:
            raise CodegenError(
                f"port {port.name!r} has direction {port.direction!r}; "
                "the current DPI transport only supports input and output ports"
            )
        if port.width > 32 and port.four_state:
            raise CodegenError(
                f"port {port.name!r} is {port.width} bits wide and four-state; "
                "wide transport currently requires a two-state bit port"
            )


def validate_internals(internals: list[Internal]) -> None:
    seen_cpp_paths: dict[tuple[str, ...], str] = {}
    for internal in internals:
        if internal.type_kind != "integral" or internal.width < 1:
            raise CodegenError(
                f"internal {internal.hdl_path!r} uses unsupported elaborated "
                f"type {internal.type_kind!r}; only packed integral variables, "
                "nets, and fixed memories are supported"
            )
        if len(internal.unpacked) > 1:
            raise CodegenError(
                f"internal {internal.hdl_path!r} has "
                f"{len(internal.unpacked)} unpacked dimensions; V1 supports one"
            )
        if internal.access not in {"read", "read_write"}:
            raise CodegenError(
                f"internal {internal.hdl_path!r} has unsupported access "
                f"{internal.access!r}"
            )
        if not isinstance(internal.forceable, bool):
            raise CodegenError(
                f"internal {internal.hdl_path!r} forceable must be a boolean"
            )
        if internal.forceable and internal.unpacked:
            if internal.symbol_kind != "variable":
                raise CodegenError(
                    f"internal {internal.hdl_path!r} is a "
                    f"{internal.symbol_kind}; forceable memories require a variable"
                )
            if internal.unpacked[0].size > 1024:
                raise CodegenError(
                    f"internal {internal.hdl_path!r} has "
                    f"{internal.unpacked[0].size} forceable memory elements; "
                    "the generated constant-index dispatch limit is 1024"
                )
        if internal.writable and internal.symbol_kind != "variable":
            raise CodegenError(
                f"internal {internal.hdl_path!r} is a {internal.symbol_kind}; "
                "read_write access requires a variable"
            )
        if not internal.cpp_path or internal.cpp_path[0] != "internal":
            raise CodegenError(
                f"internal {internal.hdl_path!r} must map below dut.internal"
            )
        for component in internal.cpp_path:
            validate_identifier(
                component,
                f"C++ path component for internal {internal.hdl_path!r}",
            )
        previous = seen_cpp_paths.setdefault(internal.cpp_path, internal.hdl_path)
        if previous != internal.hdl_path:
            raise CodegenError(
                f"internals {previous!r} and {internal.hdl_path!r} map to the "
                f"same C++ path {'.'.join(internal.cpp_path)!r}"
            )


def build_tree(items: list[Port | Internal]) -> TreeNode:
    root = TreeNode(())
    for item in items:
        node = root
        for component in item.cpp_path[:-1]:
            existing = node.children.get(component)
            if isinstance(existing, (Port, Internal)):
                raise CodegenError(
                    f"C++ path {'.'.join(item.cpp_path)!r} traverses a value"
                )
            if existing is None:
                existing = TreeNode(node.path + (component,))
                node.children[component] = existing
            node = existing
        leaf_name = item.cpp_path[-1]
        if leaf_name in node.children:
            raise CodegenError(f"duplicate C++ member path {'.'.join(item.cpp_path)!r}")
        node.children[leaf_name] = item
    return root


def pascal_case(value: str) -> str:
    parts = re.findall(r"[A-Za-z]+|\d+", value.replace("_", " "))
    return "".join(part[:1].upper() + part[1:].lower() for part in parts)


CPP_KEYWORDS = frozenset(
    {
        "alignas",
        "alignof",
        "and",
        "asm",
        "auto",
        "bitand",
        "bitor",
        "bool",
        "break",
        "case",
        "catch",
        "char",
        "class",
        "compl",
        "concept",
        "const",
        "consteval",
        "constexpr",
        "constinit",
        "const_cast",
        "continue",
        "co_await",
        "co_return",
        "co_yield",
        "decltype",
        "default",
        "delete",
        "do",
        "double",
        "dynamic_cast",
        "else",
        "enum",
        "explicit",
        "export",
        "extern",
        "false",
        "float",
        "for",
        "friend",
        "goto",
        "if",
        "inline",
        "int",
        "long",
        "mutable",
        "namespace",
        "new",
        "noexcept",
        "not",
        "nullptr",
        "operator",
        "or",
        "private",
        "protected",
        "public",
        "register",
        "reinterpret_cast",
        "requires",
        "return",
        "short",
        "signed",
        "sizeof",
        "static",
        "static_assert",
        "static_cast",
        "struct",
        "switch",
        "template",
        "this",
        "thread_local",
        "throw",
        "true",
        "try",
        "typedef",
        "typeid",
        "typename",
        "union",
        "unsigned",
        "using",
        "virtual",
        "void",
        "volatile",
        "wchar_t",
        "while",
        "xor",
    }
)


def cpp_identifier(value: str, *, pascal: bool = False) -> str:
    if pascal:
        result = pascal_case(value)
    else:
        result = re.sub(r"[^A-Za-z0-9_]", "_", value)
    if not result:
        result = "Value" if pascal else "field"
    if result[0].isdigit():
        result = ("Type" if pascal else "field_") + result
    if result in CPP_KEYWORDS:
        result += "_"
    return result


@dataclass(frozen=True)
class PackedCppType:
    data_type: PackedEnumType | PackedStructType
    base_name: str

    @property
    def value_name(self) -> str:
        return f"{self.base_name}Value"

    @property
    def view_name(self) -> str:
        return f"{self.base_name}View"


class PackedCppRegistry:
    def __init__(self) -> None:
        self.entries: list[PackedCppType] = []
        self._by_signature: dict[tuple, PackedCppType] = {}
        self._signatures_by_name: dict[str, tuple] = {}

    def register(self, data_type: PackedType, context: tuple[str, ...]) -> None:
        if isinstance(data_type, PackedIntegralType):
            return
        if isinstance(data_type, PackedUnionType):
            joined = ".".join(context)
            raise CodegenError(
                f"packed union at {joined!r} has no generated C++ typed view"
            )
        for field_info in data_type.fields if isinstance(
            data_type, PackedStructType
        ) else ():
            self.register(field_info.data_type, (*context, field_info.name))

        signature = data_type.structural_signature()
        if signature in self._by_signature:
            return
        declared_name = data_type.declared_name
        if declared_name:
            source_name = re.split(r"::|\.", declared_name)[-1]
        else:
            source_name = "_".join(context)
        base_name = cpp_identifier(source_name, pascal=True)
        previous = self._signatures_by_name.get(base_name)
        if previous is not None and previous != signature:
            raise CodegenError(
                f"packed types map to incompatible C++ name {base_name!r}"
            )
        entry = PackedCppType(data_type, base_name)
        self._signatures_by_name[base_name] = signature
        self._by_signature[signature] = entry
        self.entries.append(entry)

    def lookup(self, data_type: PackedType) -> PackedCppType:
        try:
            return self._by_signature[data_type.structural_signature()]
        except KeyError as error:
            raise CodegenError("packed C++ type was not registered") from error


def collect_packed_cpp_types(ports: list[Port]) -> PackedCppRegistry:
    registry = PackedCppRegistry()
    for port in ports:
        if port.packed_type is None:
            continue
        registry.register(port.packed_type, port.cpp_path or (port.name,))
    return registry


def signal_id(port_name: str) -> str:
    return "kSignal" + pascal_case(port_name)


def internal_export_name(
    manifest: dict[str, Any], index: int, operation: str
) -> str:
    return f"{manifest['top_module']}_internal_{index}_{operation}"


def port_word_count(port: Port) -> int:
    count = (port.width + 31) // 32
    for dimension in port.unpacked:
        count *= dimension.size
    return count


def element_word_count(port: Port) -> int:
    return (port.width + 31) // 32


def signal_word_offsets(ports: list[Port]) -> list[int]:
    offsets = [0]
    for port in ports:
        offsets.append(offsets[-1] + port_word_count(port))
    return offsets


def directional_transport_offsets(ports: list[Port]) -> dict[str, int]:
    offsets: dict[str, int] = {}
    offset = 0
    for port in ports:
        offsets[port.name] = offset
        offset += port_word_count(port)
    return offsets


def signal_word_ids(ports: list[Port]) -> list[str]:
    ids: list[str] = []
    for port in ports:
        base = signal_id(port.name)
        for word in range(port_word_count(port)):
            ids.append(base if word == 0 else f"{base} + {word}")
    return ids


def driven_port_names(ports: list[Port], manifest: dict[str, Any]) -> set[str]:
    generated_clocks = generated_clock_names(manifest)
    return {
        port.name
        for port in ports
        if port.direction == "input" and port.name not in generated_clocks
    }


def edge_observer_ports(
    ports: list[Port], manifest: dict[str, Any]
) -> list[Port]:
    configured = manifest.get("edge_observers", [])
    if not isinstance(configured, list) or not all(
        isinstance(name, str) and name for name in configured
    ):
        raise CodegenError("manifest edge_observers must be a list of port names")
    if len(configured) != len(set(configured)):
        raise CodegenError("manifest edge_observers contains a duplicate port")

    by_name = {port.name: port for port in ports}
    clock_names = {clock["port"] for clock in clock_configs(manifest)}
    observers: list[Port] = []
    for name in configured:
        port = by_name.get(name)
        if port is None:
            raise CodegenError(f"edge observer port {name!r} was not found")
        if name in clock_names:
            raise CodegenError(
                f"edge observer port {name!r} is already a configured clock"
            )
        if port.direction != "output":
            raise CodegenError(
                f"edge observer port {name!r} must be a DUT output"
            )
        if port.unpacked:
            raise CodegenError(
                f"edge observer port {name!r} cannot be an unpacked array"
            )
        if port.width != 1:
            raise CodegenError(
                f"edge observer port {name!r} must be one bit wide in V1"
            )
        observers.append(port)
    return observers


def cpp_signal_type(port: Port, driven_names: set[str]) -> str:
    direction = "Driven" if port.name in driven_names else "Observed"
    if port.unpacked:
        if len(port.unpacked) > 1:
            dimensions = ", ".join(
                f"coro::ArrayDimension<{dimension.left}, {dimension.right}>"
                for dimension in port.unpacked
            )
            return f"coro::{direction}FixedArray<{port.width}, {dimensions}>"
        declared_range = port.unpacked[0]
        return (
            f"coro::{direction}Array<{port.width}, {declared_range.left}, "
            f"{declared_range.right}>"
        )
    if port.width <= 32:
        return "coro::Signal"
    return f"coro::{direction}Signal<{port.width}>"


def cpp_internal_type(internal: Internal) -> str:
    writable = "true" if internal.writable else "false"
    force_suffix = ", true" if internal.forceable else ""
    if internal.unpacked:
        declared_range = internal.unpacked[0]
        return (
            f"probe::MemoryProbe<{internal.width}, {declared_range.left}, "
            f"{declared_range.right}, {writable}{force_suffix}>"
        )
    return f"probe::Probe<{internal.width}, {writable}{force_suffix}>"


def node_type(node: TreeNode, manifest: dict[str, Any]) -> str:
    path = ".".join(node.path)
    explicit = manifest.get("type_names", {}).get(path)
    if explicit:
        validate_identifier(explicit, f"type name for path {path!r}")
        return explicit
    if not node.path:
        return manifest["root_type"]
    return "".join(pascal_case(item) for item in node.path) + "Dut"


def node_signature(node: TreeNode, manifest: dict[str, Any]) -> tuple[Any, ...]:
    signature = []
    generated_clocks = generated_clock_names(manifest)
    for name, child in node.children.items():
        if isinstance(child, Port):
            writable = child.direction == "input" and child.name not in generated_clocks
            dimensions = tuple(
                (dimension.left, dimension.right) for dimension in child.unpacked
            )
            signature.append((name, "signal", child.width, dimensions, writable))
        elif isinstance(child, Internal):
            dimensions = tuple(
                (dimension.left, dimension.right) for dimension in child.unpacked
            )
            signature.append(
                (
                    name,
                    "internal",
                    child.width,
                    dimensions,
                    child.writable,
                    child.forceable,
                )
            )
        else:
            signature.append((name, node_type(child, manifest)))
    return tuple(signature)


def collect_structs(root: TreeNode, manifest: dict[str, Any]) -> list[TreeNode]:
    ordered: list[TreeNode] = []
    by_type: dict[str, tuple[Any, ...]] = {}

    def visit(node: TreeNode) -> None:
        for child in node.children.values():
            if isinstance(child, TreeNode):
                visit(child)
        type_name = node_type(node, manifest)
        signature = node_signature(node, manifest)
        previous = by_type.get(type_name)
        if previous is not None:
            if previous != signature:
                raise CodegenError(
                    f"C++ type {type_name!r} is assigned to incompatible hierarchy nodes"
                )
            return
        by_type[type_name] = signature
        ordered.append(node)

    visit(root)
    return ordered


def generated_banner(source: str) -> str:
    return f"// Generated by cpptb/codegen/generate_dpi_bindings.py from {source}.\n// Do not edit by hand.\n"


def packed_signal_value_type(width: int) -> str:
    if width <= 32:
        return "std::uint32_t"
    if width <= 64:
        return "std::uint64_t"
    return f"cpptb::Bits<{width}>"


def packed_value_common_lines(value_name: str, width: int) -> list[str]:
    lines = [
        f"    using raw_type = cpptb::Bits<{width}>;",
        f"    using signal_value_type = {packed_signal_value_type(width)};",
        f"    static constexpr std::size_t width = {width};",
        "",
        f"    constexpr {value_name}() = default;",
        f"    static constexpr {value_name} from_raw_bits(raw_type bits) {{",
        f"        return {value_name}{{bits}};",
        "    }",
        "    static constexpr auto from_signal_value(signal_value_type value) {",
    ]
    if width <= 64:
        lines.append("        return from_raw_bits(raw_type::from_uint(value));")
    else:
        lines.append("        return from_raw_bits(value);")
    lines.extend(
        [
            "    }",
            "    [[nodiscard]] constexpr raw_type raw_bits() const { return bits_; }",
            "    [[nodiscard]] constexpr signal_value_type signal_value() const {",
        ]
    )
    if width <= 32:
        lines.append("        return static_cast<std::uint32_t>(bits_.to_uint());")
    elif width <= 64:
        lines.append("        return bits_.to_uint64();")
    else:
        lines.append("        return bits_;")
    lines.extend(["    }", ""])
    return lines


def enum_literal(value: int, signed: bool) -> str:
    if signed:
        if value < -(1 << 63) or value >= (1 << 63):
            raise CodegenError(f"signed enum value {value} does not fit std::int64_t")
        return str(value)
    if value < 0 or value >= (1 << 64):
        raise CodegenError(f"unsigned enum value {value} does not fit std::uint64_t")
    return f"{value}ULL"


def render_packed_enum(entry: PackedCppType) -> list[str]:
    data_type = entry.data_type
    assert isinstance(data_type, PackedEnumType)
    if data_type.width > 64:
        raise CodegenError(
            f"packed enum {entry.base_name!r} is wider than C++ enum storage"
        )
    underlying = "std::int64_t" if data_type.signed else "std::uint64_t"
    lines = [f"enum class {entry.base_name} : {underlying} {{"]
    names: set[str] = set()
    for item in data_type.values:
        name = cpp_identifier(item.name, pascal=True)
        if name in names:
            raise CodegenError(
                f"packed enum {entry.base_name!r} has duplicate C++ value {name!r}"
            )
        names.add(name)
        lines.append(
            f"    {name} = {enum_literal(item.value, data_type.signed)},"
        )
    value_name = entry.value_name
    lines.extend(["};", "", f"class {value_name} {{", "public:"])
    lines.extend(packed_value_common_lines(value_name, data_type.width))
    lines.extend(
        [
            f"    static constexpr {value_name} from_enum({entry.base_name} value) {{",
            "        const auto encoded = static_cast<std::uint64_t>(",
        ]
    )
    if data_type.signed:
        lines.append("            static_cast<std::int64_t>(value));")
    else:
        lines.append("            value);")
    lines.extend(
        [
            "        return from_raw_bits(raw_type::from_uint(encoded));",
            "    }",
            f"    [[nodiscard]] constexpr bool is({entry.base_name} value) const {{",
            "        return bits_ == from_enum(value).bits_;",
            "    }",
            "",
            "private:",
            f"    constexpr explicit {value_name}(raw_type bits) : bits_(bits) {{}}",
            "    raw_type bits_{};",
            "};",
            "",
            f"[[nodiscard]] inline constexpr {value_name} to_value(",
            f"    {entry.base_name} value) {{",
            f"    return {value_name}::from_enum(value);",
            "}",
            "",
        ]
    )
    return lines


def packed_field_value_type(
    field_info: PackedField, registry: PackedCppRegistry
) -> str:
    if isinstance(field_info.data_type, PackedIntegralType):
        return f"cpptb::Bits<{field_info.width}>"
    return registry.lookup(field_info.data_type).value_name


def packed_field_get_expression(
    field_info: PackedField,
    registry: PackedCppRegistry,
    slice_expression: str,
) -> str:
    if isinstance(field_info.data_type, PackedIntegralType):
        return slice_expression
    value_name = registry.lookup(field_info.data_type).value_name
    return f"{value_name}::from_raw_bits({slice_expression})"


def packed_field_raw_expression(field_info: PackedField, value: str) -> str:
    if isinstance(field_info.data_type, PackedIntegralType):
        return value
    return f"{value}.raw_bits()"


def validate_packed_field_names(entry: PackedCppType) -> dict[PackedField, str]:
    data_type = entry.data_type
    assert isinstance(data_type, PackedStructType)
    names: dict[PackedField, str] = {}
    used: set[str] = set()
    for field_info in data_type.fields:
        name = cpp_identifier(field_info.name)
        if name in used:
            raise CodegenError(
                f"packed struct {entry.base_name!r} has duplicate C++ field {name!r}"
            )
        used.add(name)
        names[field_info] = name
    return names


def render_packed_struct(
    entry: PackedCppType, registry: PackedCppRegistry
) -> list[str]:
    data_type = entry.data_type
    assert isinstance(data_type, PackedStructType)
    value_name = entry.value_name
    view_name = entry.view_name
    field_names = validate_packed_field_names(entry)
    lines = [
        f"template <std::size_t StorageWidth> class {view_name};",
        "",
        f"class {value_name} {{",
        "public:",
    ]
    lines.extend(packed_value_common_lines(value_name, data_type.width))
    for field_info in data_type.fields:
        name = field_names[field_info]
        field_type = packed_field_value_type(field_info, registry)
        sliced = f"bits_.slice<{field_info.width}>({field_info.bit_offset})"
        getter = packed_field_get_expression(field_info, registry, sliced)
        lines.extend(
            [
                f"    [[nodiscard]] constexpr {field_type} {name}() const {{",
                f"        return {getter};",
                "    }",
                f"    constexpr {value_name}& set_{name}({field_type} value) {{",
                f"        bits_.set_slice<{field_info.width}>(",
                f"            {field_info.bit_offset}, "
                f"{packed_field_raw_expression(field_info, 'value')});",
                "        return *this;",
                "    }",
            ]
        )
        if isinstance(field_info.data_type, PackedEnumType):
            enum_name = registry.lookup(field_info.data_type).base_name
            lines.extend(
                [
                    f"    constexpr {value_name}& set_{name}({enum_name} value) {{",
                    f"        return set_{name}({field_type}::from_enum(value));",
                    "    }",
                ]
            )
        lines.append("")
    lines.extend(
        [
            f"    [[nodiscard]] constexpr {view_name}<{data_type.width}> view();",
            "",
            "private:",
            f"    constexpr explicit {value_name}(raw_type bits) : bits_(bits) {{}}",
            "    raw_type bits_{};",
            "};",
            "",
            f"template <std::size_t StorageWidth> class {view_name} {{",
            f"    static_assert(StorageWidth >= {data_type.width});",
            "",
            "public:",
            f"    constexpr explicit {view_name}(cpptb::Bits<StorageWidth>& storage,",
            "                            std::size_t bit_offset = 0)",
            "        : storage_(&storage), bit_offset_(bit_offset) {}",
            "",
            f"    [[nodiscard]] constexpr cpptb::Bits<{data_type.width}> raw_bits() const {{",
            f"        return storage_->template slice<{data_type.width}>(bit_offset_);",
            "    }",
            f"    constexpr {view_name}& set_raw_bits(",
            f"        cpptb::Bits<{data_type.width}> value) {{",
            f"        storage_->template set_slice<{data_type.width}>(bit_offset_, value);",
            "        return *this;",
            "    }",
            "",
        ]
    )
    for field_info in data_type.fields:
        name = field_names[field_info]
        field_type = packed_field_value_type(field_info, registry)
        offset = f"bit_offset_ + {field_info.bit_offset}"
        if isinstance(field_info.data_type, PackedStructType):
            nested = registry.lookup(field_info.data_type)
            lines.extend(
                [
                    f"    [[nodiscard]] constexpr {nested.view_name}<StorageWidth> "
                    f"{name}() {{",
                    f"        return {nested.view_name}<StorageWidth>{{*storage_, {offset}}};",
                    "    }",
                    f"    [[nodiscard]] constexpr {field_type} {name}() const {{",
                    f"        return {field_type}::from_raw_bits(",
                    f"            storage_->template slice<{field_info.width}>({offset}));",
                    "    }",
                ]
            )
        else:
            sliced = (
                f"storage_->template slice<{field_info.width}>({offset})"
            )
            getter = packed_field_get_expression(field_info, registry, sliced)
            lines.extend(
                [
                    f"    [[nodiscard]] constexpr {field_type} {name}() const {{",
                    f"        return {getter};",
                    "    }",
                ]
            )
        lines.extend(
            [
                f"    constexpr {view_name}& set_{name}({field_type} value) {{",
                f"        storage_->template set_slice<{field_info.width}>(",
                f"            {offset}, {packed_field_raw_expression(field_info, 'value')});",
                "        return *this;",
                "    }",
            ]
        )
        if isinstance(field_info.data_type, PackedEnumType):
            enum_name = registry.lookup(field_info.data_type).base_name
            lines.extend(
                [
                    f"    constexpr {view_name}& set_{name}({enum_name} value) {{",
                    f"        return set_{name}({field_type}::from_enum(value));",
                    "    }",
                ]
            )
        lines.append("")
    lines.extend(
        [
            "private:",
            "    cpptb::Bits<StorageWidth>* storage_;",
            "    std::size_t bit_offset_;",
            "};",
            "",
            f"[[nodiscard]] inline constexpr {view_name}<{data_type.width}> "
            f"{value_name}::view() {{",
            f"    return {view_name}<{data_type.width}>{{bits_}};",
            "}",
            "",
        ]
    )
    return lines


def render_packed_cpp_types(registry: PackedCppRegistry) -> list[str]:
    lines: list[str] = []
    for entry in registry.entries:
        if isinstance(entry.data_type, PackedEnumType):
            lines.extend(render_packed_enum(entry))
        else:
            lines.extend(render_packed_struct(entry, registry))
    return lines


def render_cpp_dut(
    ports: list[Port],
    internals: list[Internal],
    root: TreeNode,
    manifest: dict[str, Any],
    source: str,
) -> str:
    packed_types = collect_packed_cpp_types(ports)
    lines = [
        generated_banner(source).rstrip(),
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        '#include "cpptb/coro_runtime.hpp"',
    ]
    if internals:
        lines.append('#include "cpptb/probe.hpp"')
    lines.extend(
        [
            "",
            f"namespace {manifest['namespace']} {{",
            "",
            "enum SignalId : uint32_t {",
        ]
    )
    offsets = signal_word_offsets(ports)
    driven_names = driven_port_names(ports, manifest)
    if all(port_word_count(port) == 1 for port in ports):
        lines.extend(f"    {signal_id(port.name)}," for port in ports)
        lines.extend(["    kSignalCount,", "};", ""])
    else:
        lines.extend(
            f"    {signal_id(port.name)} = {offset},"
            for port, offset in zip(ports, offsets)
        )
        lines.extend([f"    kSignalCount = {offsets[-1]},", "};", ""])

    lines.extend(render_packed_cpp_types(packed_types))

    for node in collect_structs(root, manifest):
        lines.append(f"struct {node_type(node, manifest)} {{")
        for name, child in node.children.items():
            field_type = (
                cpp_signal_type(child, driven_names)
                if isinstance(child, Port)
                else (
                    cpp_internal_type(child)
                    if isinstance(child, Internal)
                    else node_type(child, manifest)
                )
            )
            lines.append(f"    {field_type} {name};")
        lines.extend(["};", ""])

    lines.append(f"}}  // namespace {manifest['namespace']}")
    lines.append("")
    return "\n".join(lines)


def render_binding_expr(
    node: TreeNode,
    driven_names: set[str],
    internal_indices: dict[Internal, int],
    indent: int = 4,
) -> list[str]:
    prefix = " " * indent
    lines = [prefix + "{"]
    values = list(node.children.values())
    for index, child in enumerate(values):
        comma = "," if index + 1 < len(values) else ""
        if isinstance(child, Port):
            writable = "true" if child.name in driven_names else "false"
            if child.unpacked:
                declared_range = child.unpacked[0]
                if len(child.unpacked) == 1:
                    expression = (
                        f"make_signal(coro::ArraySpec<{child.width}, "
                        f"{declared_range.left}, {declared_range.right}, "
                        f"{writable}>{{}}, {signal_id(child.name)}, "
                        f'"{child.name}")'
                    )
                else:
                    dimensions = ", ".join(
                        f"coro::ArrayDimension<{dimension.left}, "
                        f"{dimension.right}>"
                        for dimension in child.unpacked
                    )
                    inner_words = element_word_count(child)
                    for dimension in child.unpacked[1:]:
                        inner_words *= dimension.size
                    transport_width = inner_words * 32
                    expression = (
                        f"coro::reshape_fixed_array(coro::FixedArraySpec<"
                        f"{child.width}, {writable}, {dimensions}>{{}}, "
                        f"make_signal(coro::ArraySpec<{transport_width}, "
                        f"{declared_range.left}, {declared_range.right}, "
                        f"{writable}>{{}}, {signal_id(child.name)}, "
                        f'"{child.name}"))'
                    )
                lines.append(" " * (indent + 4) + expression + comma)
            elif child.width <= 32:
                lines.append(
                    " " * (indent + 4)
                    + f'make_signal({signal_id(child.name)}, "{child.name}"){comma}'
                )
            else:
                lines.append(
                    " " * (indent + 4)
                    + f"make_signal(coro::SignalSpec<{child.width}, {writable}>{{}}, "
                    + f'{signal_id(child.name)}, "{child.name}"){comma}'
                )
        elif isinstance(child, Internal):
            lines.append(
                " " * (indent + 4)
                + f"make_internal_{internal_indices[child]}(){comma}"
            )
        else:
            child_lines = render_binding_expr(
                child, driven_names, internal_indices, indent + 4
            )
            child_lines[-1] += comma
            lines.extend(child_lines)
    lines.append(prefix + "}")
    return lines


def cpp_internal_export_declarations(
    internals: list[Internal], manifest: dict[str, Any]
) -> list[str]:
    lines: list[str] = []
    for index, internal in enumerate(internals):
        get_name = internal_export_name(manifest, index, "get")
        index_arg = "int index" if internal.unpacked else ""
        if internal.width <= 32:
            lines.append(f"unsigned int {get_name}({index_arg});")
        elif internal.width <= 64:
            lines.append(f"unsigned long long {get_name}({index_arg});")
        else:
            separator = ", " if index_arg else ""
            lines.append(
                f"void {get_name}({index_arg}{separator}svBitVecVal* value);"
            )
        index_prefix = f"{index_arg}, " if index_arg else ""
        value_parameter = (
            "unsigned int value"
            if internal.width <= 32
            else (
                "unsigned long long value"
                if internal.width <= 64
                else "const svBitVecVal* value"
            )
        )
        if internal.writable:
            deposit_name = internal_export_name(manifest, index, "deposit")
            lines.append(
                f"void {deposit_name}({index_prefix}{value_parameter});"
            )
        if internal.forceable:
            force_name = internal_export_name(manifest, index, "force")
            release_name = internal_export_name(manifest, index, "release")
            lines.append(f"void {force_name}({index_prefix}{value_parameter});")
            lines.append(f"void {release_name}({index_arg});")
    return lines


def cpp_internal_helpers(
    internals: list[Internal], manifest: dict[str, Any]
) -> list[str]:
    lines: list[str] = []
    for index, internal in enumerate(internals):
        get_name = internal_export_name(manifest, index, "get")
        index_arg = "index" if internal.unpacked else ""
        value_type = f"probe::Value<{internal.width}>"
        lines.append(
            f"inline {value_type} internal_{index}_get(int32_t index) {{"
        )
        if internal.width <= 32:
            lines.append(f"    return {get_name}({index_arg});")
        elif internal.width <= 64:
            lines.append(f"    return {get_name}({index_arg});")
        else:
            lines.extend(
                [
                    f"    {value_type}::word_array words{{}};",
                    f"    {get_name}({index_arg}{', ' if index_arg else ''}"
                    "reinterpret_cast<svBitVecVal*>(words.data()));",
                    f"    return {value_type}::from_words(words);",
                ]
            )
        lines.extend(["}", ""])

        if internal.writable:
            deposit_name = internal_export_name(manifest, index, "deposit")
            lines.append(
                f"inline void internal_{index}_deposit(int32_t index, "
                f"{value_type} value) {{"
            )
            value_prefix = f"{index_arg}, " if index_arg else ""
            if internal.width <= 64:
                lines.append(f"    {deposit_name}({value_prefix}value);")
            else:
                lines.append(
                    f"    {deposit_name}({value_prefix}"
                    "reinterpret_cast<const svBitVecVal*>(value.words().data()));"
                )
            lines.extend(["}", ""])

        if internal.forceable:
            force_name = internal_export_name(manifest, index, "force")
            release_name = internal_export_name(manifest, index, "release")
            value_prefix = f"{index_arg}, " if index_arg else ""
            lines.append(
                f"inline void internal_{index}_force(int32_t index, "
                f"{value_type} value) {{"
            )
            if internal.width <= 64:
                lines.append(f"    {force_name}({value_prefix}value);")
            else:
                lines.append(
                    f"    {force_name}({value_prefix}"
                    "reinterpret_cast<const svBitVecVal*>(value.words().data()));"
                )
            lines.extend(["}", ""])
            lines.extend(
                [
                    f"inline void internal_{index}_release(int32_t index) {{",
                    f"    {release_name}({index_arg});",
                    "}",
                    "",
                ]
            )

        writable = "true" if internal.writable else "false"
        force_suffix = ", true" if internal.forceable else ""
        deposit_callback = (
            f"internal_{index}_deposit" if internal.writable else "nullptr"
        )
        force_callback = (
            f"internal_{index}_force" if internal.forceable else "nullptr"
        )
        release_callback = (
            f"internal_{index}_release" if internal.forceable else "nullptr"
        )
        lines.append(f"inline auto make_internal_{index}() {{")
        if internal.unpacked:
            declared_range = internal.unpacked[0]
            lines.append(
                f"    return probe::MemoryProbe<{internal.width}, "
                f"{declared_range.left}, {declared_range.right}, {writable}"
                f"{force_suffix}>{{"
            )
            lines.append(
                f'        "{internal.hdl_path}", internal_{index}_get, '
                f"{deposit_callback}, {force_callback}, {release_callback}}};"
            )
        else:
            lines.append(
                f"    return probe::Probe<{internal.width}, {writable}"
                f"{force_suffix}>{{"
            )
            lines.append(
                f'        0, "{internal.hdl_path}", internal_{index}_get, '
                f"{deposit_callback}, {force_callback}, {release_callback}}};"
            )
        lines.extend(["}", ""])
    return lines


def render_cpp_binding(
    ports: list[Port],
    internals: list[Internal],
    root: TreeNode,
    manifest: dict[str, Any],
    source: str,
) -> str:
    clock_names = {clock["port"] for clock in clock_configs(manifest)}
    edge_observers = edge_observer_ports(ports, manifest)
    driven_names = driven_port_names(ports, manifest)
    compact_input_transport = bool(
        manifest.get("run", {}).get("compact_input_transport", True)
    )
    driven = [port for port in ports if port.name in driven_names]
    observed = [port for port in ports if port.name not in driven_names]
    offsets = signal_word_offsets(ports)
    include = manifest["outputs"]["cpp_include"]
    lines = [
        generated_banner(source).rstrip(),
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstdint>",
        "#include <utility>",
    ]
    if internals:
        lines.append('#include "svdpi.h"')
    lines.extend(["", f'#include "{include}"', ""])
    if internals:
        lines.extend(['extern "C" {'])
        lines.extend(
            f"    {declaration}"
            for declaration in cpp_internal_export_declarations(internals, manifest)
        )
        lines.extend(["}", ""])
    lines.extend(
        [
            f"namespace {manifest['namespace']}::generated {{",
            "",
            f"inline constexpr bool kCompactInputTransport = "
            f"{'true' if compact_input_transport else 'false'};",
            f"inline constexpr std::array<uint32_t, {len(clock_names)}> kClockSignalIds = {{",
        ]
    )
    lines.extend(
        f"    {signal_id(clock['port'])}," for clock in clock_configs(manifest)
    )
    lines.extend([
        "};",
        f"inline constexpr std::array<uint32_t, {len(edge_observers)}> "
        "kEdgeObserverSignalIds = {",
    ])
    lines.extend(f"    {signal_id(port.name)}," for port in edge_observers)
    lines.extend([
        "};",
        f"inline constexpr std::array<std::pair<uint32_t, uint32_t>, {len(driven)}> "
        "kDrivenSignalSpans = {{",
    ])
    lines.extend(
        f"    {{{signal_id(port.name)}, {port_word_count(port)}}}," for port in driven
    )
    lines.extend([
        "}};",
        f"inline constexpr std::array<uint32_t, {sum(port_word_count(port) for port in observed)}> "
        "kObservedSignalWordIds = {",
    ])
    lines.extend(f"    {word_id}," for word_id in signal_word_ids(observed))
    lines.extend([
        "};",
        f"inline constexpr std::array<uint32_t, {sum(port_word_count(port) for port in driven)}> "
        "kDrivenSignalWordIds = {",
    ])
    lines.extend(f"    {word_id}," for word_id in signal_word_ids(driven))
    lines.extend(["};", ""])
    if internals:
        lines.extend(cpp_internal_helpers(internals, manifest))
    lines.append("template <typename MakeSignal>")
    lines.append(
        f"{manifest['root_type']} bind_dut(MakeSignal&& make_signal) {{"
    )
    expression = render_binding_expr(
        root,
        driven_names,
        {internal: index for index, internal in enumerate(internals)},
        4,
    )
    expression[0] = "    return " + expression[0].lstrip()
    expression[-1] += ";"
    lines.extend(expression)
    lines.extend(["}", "", f"}}  // namespace {manifest['namespace']}::generated", ""])
    return "\n".join(lines)


def sv_decl(port: Port) -> str:
    packed = "" if port.width == 1 else f" [{port.width - 1}:0]"
    value_type = "bit" if port.width > 32 else "logic"
    unpacked = "".join(
        f" [{dimension.left}:{dimension.right}]" for dimension in port.unpacked
    )
    return f"  {value_type}{packed} {port.name}{unpacked};"


def sv_array_loops(port: Port) -> tuple[list[str], list[str], str, str]:
    indices = (
        [f"cpptb_{port.name}_index"]
        if len(port.unpacked) == 1
        else [
            f"cpptb_{port.name}_index_{rank}"
            for rank in range(len(port.unpacked))
        ]
    )
    lines = []
    for rank, (dimension, index) in enumerate(zip(port.unpacked, indices)):
        lines.append(
            "    " + "  " * rank
            + f"for (int {index} = {dimension.low}; "
            + f"{index} <= {dimension.high}; {index}++) begin"
        )
    source = port.name + "".join(f"[{index}]" for index in indices)
    linear = f"({indices[0]} - {port.unpacked[0].low})"
    for dimension, index in zip(port.unpacked[1:], indices[1:]):
        linear = f"({linear} * {dimension.size} + ({index} - {dimension.low}))"
    return lines, indices, source, linear


def sv_pack_assignments(port: Port, signal: str | None = None) -> list[str]:
    signal = signal or sv_signal_constant(port)
    if port.unpacked:
        words = element_word_count(port)
        lines, indices, source, linear = sv_array_loops(port)
        body_indent = "    " + "  " * len(indices)
        for word in range(words):
            lsb = word * 32
            width = min(32, port.width - lsb)
            word_source = source
            if port.width > 32:
                word_source += f"[{lsb} +: {width}]"
            offset = f"{linear} * {words}"
            if word:
                offset += f" + {word}"
            lines.append(
                f"{body_indent}in_words[{signal} + {offset}] = {word_source};"
            )
        for rank in reversed(range(len(indices))):
            lines.append("    " + "  " * rank + "end")
        return lines

    lines = []
    for word in range(element_word_count(port)):
        lsb = word * 32
        width = min(32, port.width - lsb)
        if port.width <= 32:
            source = port.name
        else:
            source = f"{port.name}[{lsb} +: {width}]"
        index = signal if element_word_count(port) == 1 else f"{signal} + {word}"
        lines.append(f"    in_words[{index}] = {source};")
    return lines


def sv_output_assignments(port: Port, signal: str | None = None) -> list[str]:
    signal = signal or sv_signal_constant(port)
    if port.unpacked:
        words = element_word_count(port)
        lines, indices, target, linear = sv_array_loops(port)
        body_indent = "    " + "  " * len(indices)
        offset = f"{linear} * {words}"
        if port.width > 32:
            chunks = []
            for word in reversed(range(words)):
                lsb = word * 32
                width = min(32, port.width - lsb)
                word_offset = offset if word == 0 else f"{offset} + {word}"
                source = f"out_words[{signal} + {word_offset}]"
                if width < 32:
                    source += f"[{width - 1}:0]"
                chunks.append(source)
            lines.append(f"{body_indent}{target} = {{{', '.join(chunks)}}};")
        else:
            source = f"out_words[{signal} + {offset}]"
            if port.width == 1:
                source += "[0]"
            elif port.width < 32:
                source += f"[{port.width - 1}:0]"
            lines.append(f"{body_indent}{target} = {source};")
        for rank in reversed(range(len(indices))):
            lines.append("    " + "  " * rank + "end")
        return lines

    if port.width > 32:
        chunks = []
        for word in reversed(range(element_word_count(port))):
            lsb = word * 32
            width = min(32, port.width - lsb)
            source = f"out_words[{signal} + {word}]"
            if width < 32:
                source += f"[{width - 1}:0]"
            chunks.append(source)
        return [f"    {port.name} = {{{', '.join(chunks)}}};"]

    lines = []
    for word in range(element_word_count(port)):
        lsb = word * 32
        width = min(32, port.width - lsb)
        index = signal if element_word_count(port) == 1 else f"{signal} + {word}"
        source = f"out_words[{index}]"
        if port.width <= 32:
            if port.width == 1:
                source += "[0]"
            elif port.width < 32:
                source += f"[{port.width - 1}:0]"
            target = port.name
        else:
            target = f"{port.name}[{lsb} +: {width}]"
        lines.append(f"    {target} = {source};")
    return lines


def sv_signal_constant(port: Port) -> str:
    name = signal_id(port.name).replace("kSignal", "SIGNAL_").upper()
    return name


def sv_internal_export_functions(
    internals: list[Internal], manifest: dict[str, Any]
) -> list[str]:
    lines: list[str] = []
    for index, internal in enumerate(internals):
        target = f"i_dut.{internal.hdl_path}"
        indexed_target = f"{target}[index]" if internal.unpacked else target
        index_formal = "input int index" if internal.unpacked else ""
        value_type = (
            "int unsigned"
            if internal.width <= 32
            else (
                "longint unsigned"
                if internal.width <= 64
                else f"bit [{internal.width - 1}:0]"
            )
        )
        force_shadow = f"internal_{index}_force_shadow"
        if internal.forceable:
            packed_shadow_type = f"bit [{internal.width - 1}:0]"
            unpacked_suffix = ""
            if internal.unpacked:
                declared_range = internal.unpacked[0]
                unpacked_suffix = (
                    f" [{declared_range.left}:{declared_range.right}]"
                )
            lines.extend(
                [
                    f"  {packed_shadow_type} {force_shadow}{unpacked_suffix};",
                    "",
                ]
            )

        get_name = internal_export_name(manifest, index, "get")
        if internal.width <= 64:
            lines.extend(
                [
                    f'  export "DPI-C" function {get_name};',
                    f"  function {value_type} {get_name}({index_formal});",
                ]
            )
            if internal.unpacked and internal.forceable:
                declared_range = internal.unpacked[0]
                lines.append("    case (index)")
                for memory_index in range(
                    declared_range.low, declared_range.high + 1
                ):
                    lines.append(
                        f"      {memory_index}: {get_name} = "
                        f"{target}[{memory_index}];"
                    )
                lines.extend(
                    [
                        f'      default: $fatal(1, "read index %0d is out of bounds", index);',
                        "    endcase",
                    ]
                )
            else:
                lines.append(f"    {get_name} = {indexed_target};")
            lines.extend(["  endfunction", ""])
        else:
            separator = ", " if index_formal else ""
            lines.extend(
                [
                    f'  export "DPI-C" function {get_name};',
                    f"  function void {get_name}({index_formal}{separator}"
                    f"output {value_type} value);",
                ]
            )
            if internal.unpacked and internal.forceable:
                declared_range = internal.unpacked[0]
                lines.append("    case (index)")
                for memory_index in range(
                    declared_range.low, declared_range.high + 1
                ):
                    lines.append(
                        f"      {memory_index}: value = {target}[{memory_index}];"
                    )
                lines.extend(
                    [
                        f'      default: $fatal(1, "read index %0d is out of bounds", index);',
                        "    endcase",
                    ]
                )
            else:
                lines.append(f"    value = {indexed_target};")
            lines.extend(["  endfunction", ""])
        separator = ", " if index_formal else ""
        if internal.writable:
            deposit_name = internal_export_name(manifest, index, "deposit")
            lines.extend(
                [
                    f'  export "DPI-C" function {deposit_name};',
                    f"  function void {deposit_name}({index_formal}{separator}"
                    f"input {value_type} value);",
                ]
            )
            if internal.unpacked and internal.forceable:
                declared_range = internal.unpacked[0]
                lines.append("    case (index)")
                for memory_index in range(
                    declared_range.low, declared_range.high + 1
                ):
                    lines.append(
                        f"      {memory_index}: {target}[{memory_index}] = value;"
                    )
                lines.extend(
                    [
                        f'      default: $fatal(1, "deposit index %0d is out of bounds", index);',
                        "    endcase",
                    ]
                )
            else:
                lines.append(f"    {indexed_target} = value;")
            lines.extend(["  endfunction", ""])

        if not internal.forceable:
            continue

        force_name = internal_export_name(manifest, index, "force")
        release_name = internal_export_name(manifest, index, "release")
        lines.extend(
            [
                f'  export "DPI-C" function {force_name};',
                f"  function void {force_name}({index_formal}{separator}"
                f"input {value_type} value);",
            ]
        )
        if internal.unpacked:
            declared_range = internal.unpacked[0]
            lines.append("    case (index)")
            for memory_index in range(
                declared_range.low, declared_range.high + 1
            ):
                lines.extend(
                    [
                        f"      {memory_index}: begin",
                        f"        {force_shadow}[{memory_index}] = value;",
                        f"        force {target}[{memory_index}] = "
                        f"{force_shadow}[{memory_index}];",
                        "      end",
                    ]
                )
            lines.extend(
                [
                    f'      default: $fatal(1, "force index %0d is out of bounds", index);',
                    "    endcase",
                ]
            )
        else:
            lines.extend(
                [
                    f"    {force_shadow} = value;",
                    f"    force {target} = {force_shadow};",
                ]
            )
        lines.extend(
            [
                "  endfunction",
                "",
                f'  export "DPI-C" function {release_name};',
                f"  function void {release_name}({index_formal});",
            ]
        )
        if internal.unpacked:
            declared_range = internal.unpacked[0]
            lines.append("    case (index)")
            for memory_index in range(
                declared_range.low, declared_range.high + 1
            ):
                lines.append(
                    f"      {memory_index}: release {target}[{memory_index}];"
                )
            lines.extend(
                [
                    f'      default: $fatal(1, "release index %0d is out of bounds", index);',
                    "    endcase",
                ]
            )
        else:
            lines.append(f"    release {target};")
        lines.extend(["  endfunction", ""])
    return lines


def render_sv(
    ports: list[Port],
    internals: list[Internal],
    manifest: dict[str, Any],
    source: str,
) -> str:
    clocks = clock_configs(manifest)
    edge_observers = edge_observer_ports(ports, manifest)
    generated_clocks = generated_clock_names(manifest)
    driven_names = driven_port_names(ports, manifest)
    run = manifest.get("run", {})
    compact_input_transport = bool(run.get("compact_input_transport", True))
    observed_ports = [port for port in ports if port.name not in driven_names]
    driven_ports = [port for port in ports if port.name in driven_names]
    all_offsets = signal_word_offsets(ports)
    global_offsets = {
        port.name: offset for port, offset in zip(ports, all_offsets)
    }
    input_offsets = (
        directional_transport_offsets(observed_ports)
        if compact_input_transport
        else {port.name: global_offsets[port.name] for port in observed_ports}
    )
    output_offsets = directional_transport_offsets(driven_ports)
    input_word_count = sum(port_word_count(port) for port in observed_ports)
    output_word_count = sum(port_word_count(port) for port in driven_ports)
    input_storage_count = max(
        1, input_word_count if compact_input_transport else all_offsets[-1]
    )
    output_storage_count = max(1, output_word_count)
    primary_clocks = [clock for clock in clocks if clock.get("primary", False)]
    if len(primary_clocks) > 1:
        raise CodegenError("only one clock may be marked primary")
    primary_clock = primary_clocks[0] if primary_clocks else (clocks[0] if clocks else None)
    init_function = run.get("init_function", "cpptb_dpi_init")
    step_function = run.get("step_function", "cpptb_dpi_step")
    pull_outputs_function = run.get(
        "pull_outputs_function", "cpptb_dpi_pull_outputs"
    )
    next_deadline_function = run.get(
        "next_deadline_function", "cpptb_dpi_next_timer_deadline"
    )
    edge_interest_function = run.get(
        "edge_interest_function", "cpptb_dpi_edge_interest"
    )
    iteration_plusarg = run.get("iteration_plusarg", "CPPTB_ITERS")
    default_iterations = int(run.get("default_iterations", 1))
    parameters = manifest.get("parameters", {})
    default_clock = clocks[0] if clocks else {}
    timeprecision = run.get(
        "timeprecision", default_clock.get("timeprecision", "1ps")
    )
    timeprecision_fs = time_literal_femtoseconds(
        timeprecision, "run.timeprecision"
    )
    offsets = signal_word_offsets(ports)

    lines = [
        generated_banner(source).rstrip(),
        f"module {manifest['top_module']};",
        f"  timeunit {timeprecision};",
        f"  timeprecision {timeprecision};",
        "",
    ]
    lines.extend(f"  localparam int {name} = {value};" for name, value in parameters.items())
    if parameters:
        lines.append("")
    lines.extend(
        [
            "  localparam int PHASE_INIT = 0;",
            "  localparam int PHASE_EDGE = 1;",
            "  localparam int PHASE_DELAY = 4;",
            f"  localparam longint unsigned TIMEPRECISION_FS = {timeprecision_fs};",
            "",
            "  localparam int EDGE_RISING = 0;",
            "  localparam int EDGE_FALLING = 1;",
            "  localparam int EDGE_ANY = 2;",
            "  localparam int unsigned NO_SIGNAL = 32'hffff_ffff;",
            "  localparam longint unsigned NO_TIMER = 64'hffff_ffff_ffff_ffff;",
            "",
            "  localparam int STEP_DONE = 1;",
            "  localparam int STEP_TIMER_CHANGED = 8;",
            "  localparam int STEP_FALLING_EDGES = 16;",
            "  localparam int STEP_OUTPUTS_CHANGED = 32;",
            "  localparam int STEP_EDGE_INTEREST_CHANGED = 64;",
            "",
        ]
    )
    for port, offset in zip(ports, offsets):
        lines.append(f"  localparam int {sv_signal_constant(port)} = {offset};")
    for port in observed_ports:
        lines.append(
            f"  localparam int INPUT_{sv_signal_constant(port)} = "
            f"{input_offsets[port.name]};"
        )
    for port in driven_ports:
        lines.append(
            f"  localparam int OUTPUT_{sv_signal_constant(port)} = "
            f"{output_offsets[port.name]};"
        )
    lines.extend(
        [
            f"  localparam int SIGNAL_COUNT = {offsets[-1]};",
            f"  localparam int INPUT_WORD_COUNT = {input_storage_count};",
            f"  localparam int OUTPUT_WORD_COUNT = {output_storage_count};",
            "",
            f'  import "DPI-C" context function void {init_function}(',
            "      input int unsigned iterations,",
            "      input longint unsigned timeprecision_fs",
            "  );",
            f'  import "DPI-C" context function int {step_function}(',
            "      input int unsigned phase,",
            "      input longint unsigned sim_time,",
            "      input longint unsigned sim_cycles,",
            "      input int unsigned event_signal_id,",
            "      input int unsigned event_edge,",
            "      input int unsigned in_words[]",
            "  );",
            f'  import "DPI-C" function void {pull_outputs_function}(',
            "      output int unsigned out_words[]",
            "  );",
            f'  import "DPI-C" context function longint unsigned {next_deadline_function}();',
            f'  import "DPI-C" context function int unsigned {edge_interest_function}(',
            "      input int unsigned signal_id",
            "  );",
            "",
        ]
    )
    lines.extend(sv_decl(port) for port in ports)
    lines.extend(
        [
            "",
            "  int unsigned iterations;",
            "  longint unsigned sim_cycles;",
            "  longint unsigned timer_generation;",
            "  longint unsigned timer_deadline;",
            "  longint unsigned timer_owner_target;",
            "  event timer_kick;",
            "  int status;",
            "  bit track_falling_edges;",
            "  int initial_requests;",
            "  int unsigned in_words[0:INPUT_WORD_COUNT-1];",
            "  int unsigned out_words[0:OUTPUT_WORD_COUNT-1];",
            "  int unsigned edge_interest[0:SIGNAL_COUNT-1];",
            "",
            "  task automatic pack_inputs();",
        ]
    )
    for port in observed_ports:
        lines.extend(
            sv_pack_assignments(port, f"INPUT_{sv_signal_constant(port)}")
        )
    lines.extend(["  endtask", "", "  task automatic apply_outputs();"])
    for port in driven_ports:
        lines.extend(
            sv_output_assignments(port, f"OUTPUT_{sv_signal_constant(port)}")
        )
    lines.extend(
        [
            "  endtask",
            "",
            "  task automatic update_status(input int requests);",
            "    if (requests < 0) begin",
            "      status = -1;",
            "    end else begin",
            "      track_falling_edges = (requests & STEP_FALLING_EDGES) != 0;",
            "      if ((requests & STEP_DONE) != 0) begin",
            "        status = 1;",
            "      end",
            "    end",
            "  endtask",
            "",
            "  task automatic run_step(",
            "      input int unsigned phase,",
            "      input int unsigned event_signal_id,",
            "      input int unsigned event_edge,",
            "      output int requests",
            "  );",
            "    pack_inputs();",
            f"    requests = {step_function}(phase, $time, sim_cycles,",
            "                               event_signal_id, event_edge,",
            "                               in_words);",
            "    if ((requests >= 0) &&",
            "        ((phase == PHASE_INIT) ||",
            "         ((requests & STEP_OUTPUTS_CHANGED) != 0))) begin",
            f"      {pull_outputs_function}(out_words);",
            "      apply_outputs();",
            "    end",
            "    update_status(requests);",
            "  endtask",
            "",
            "  task automatic timer_wakeup(",
            "      input longint unsigned deadline,",
            "      input longint unsigned generation",
            "  );",
            "    int requests;",
            "    if (deadline > $time) begin",
            "      #(deadline - $time);",
            "    end",
            "    if ((status == 0) && (generation == timer_generation)) begin",
            "      run_step(PHASE_DELAY, NO_SIGNAL, EDGE_RISING, requests);",
            "      service_requests(requests);",
            "    end",
            "  endtask",
            "",
            "  task automatic update_timer_schedule();",
            "    longint unsigned deadline;",
            "    longint unsigned generation;",
            f"    deadline = {next_deadline_function}();",
            "    timer_deadline = deadline;",
            "    timer_generation++;",
            "    generation = timer_generation;",
            "    if (deadline != NO_TIMER) begin",
            "      if (timer_owner_target == NO_TIMER) begin",
            "        -> timer_kick;",
            "      end else if (deadline < timer_owner_target) begin",
            "        fork",
            "          timer_wakeup(deadline, generation);",
            "        join_none",
            "      end",
            "    end",
            "  endtask",
            "",
            "  task automatic timer_owner();",
            "    int requests;",
            "    longint unsigned target;",
            "    while (status == 0) begin",
            "      if (timer_deadline == NO_TIMER) begin",
            "        @(timer_kick);",
            "      end else if (timer_deadline > $time) begin",
            "        target = timer_deadline;",
            "        timer_owner_target = target;",
            "        #(target - $time);",
            "        timer_owner_target = NO_TIMER;",
            "      end else begin",
            "        run_step(PHASE_DELAY, NO_SIGNAL, EDGE_RISING, requests);",
            "        service_requests(requests);",
            "      end",
            "    end",
            "  endtask",
            "",
            "  task automatic service_requests(",
            "      input int initial_requests",
            "  );",
            "    int requests;",
            "    requests = initial_requests;",
        ]
    )
    if edge_observers:
        lines.extend(
            [
                "    if ((requests & STEP_EDGE_INTEREST_CHANGED) != 0) begin",
            ]
        )
        for port in edge_observers:
            constant = sv_signal_constant(port)
            lines.append(
                f"      edge_interest[{constant}] = {edge_interest_function}({constant});"
            )
        lines.append("    end")
    lines.extend(
        [
            "    if ((requests & STEP_TIMER_CHANGED) != 0) begin",
            "      update_timer_schedule();",
            "    end",
            "  endtask",
            "",
        ]
    )

    for index, clock in enumerate(clocks):
        clock_name = clock["port"]
        if clock_source(clock) == "generated":
            lines.extend(
                [
                    f"  task automatic drive_clock_{index}();",
                    "    int requests;",
                    "    int event_edge;",
                    "    realtime next_edge;",
                ]
            )
            phase = clock.get("phase")
            if phase:
                lines.append(
                    f"    next_edge = $realtime + {phase} + "
                    f"{clock.get('half_period', '1ns')};"
                )
            else:
                lines.append(
                    f"    next_edge = $realtime + "
                    f"{clock.get('half_period', '1ns')};"
                )
            lines.extend(
                [
                    "    while (status == 0) begin",
                    "      if (next_edge > $realtime) begin",
                    "        #(next_edge - $realtime);",
                    "      end",
                    "      if (status == 0) begin",
                    f"        {clock_name} = ~{clock_name};",
                    f"        next_edge = next_edge + {clock.get('half_period', '1ns')};",
                    f"        event_edge = {clock_name} ? EDGE_RISING : EDGE_FALLING;",
                ]
            )
            if primary_clock is not None and clock_name == primary_clock["port"]:
                lines.extend(
                    [
                        "        if (event_edge == EDGE_RISING) begin",
                        "          sim_cycles++;",
                        "        end",
                    ]
                )
            lines.extend(
                [
                    "        if ((event_edge == EDGE_RISING) ||",
                    "            track_falling_edges) begin",
                    f"          run_step(PHASE_EDGE, {sv_signal_constant(next(port for port in ports if port.name == clock_name))},",
                    "                   event_edge, requests);",
                    "          service_requests(requests);",
                    "        end",
                    "      end",
                    "    end",
                    "  endtask",
                    "",
                ]
            )
            continue

        lines.extend(
            [
                f"  task automatic observe_clock_{index}();",
                "    int requests;",
                "    int event_edge;",
                "    while (status == 0) begin",
                f"      @({clock_name});",
                f"      event_edge = {clock_name} ? EDGE_RISING : EDGE_FALLING;",
            ]
        )
        if primary_clock is not None and clock_name == primary_clock["port"]:
            lines.extend(
                [
                    "      if (event_edge == EDGE_RISING) begin",
                    "        sim_cycles++;",
                    "      end",
                ]
            )
        lines.extend(
            [
                "      if ((event_edge == EDGE_RISING) || track_falling_edges) begin",
                "        if (status == 0) begin",
                f"          run_step(PHASE_EDGE, {sv_signal_constant(next(port for port in ports if port.name == clock_name))},",
                "                   event_edge, requests);",
                "          service_requests(requests);",
                "        end",
                "      end",
                "    end",
                "  endtask",
                "",
            ]
        )

    for index, port in enumerate(edge_observers):
        constant = sv_signal_constant(port)
        lines.extend(
            [
                f"  task automatic observe_signal_{index}();",
                "    int requests;",
                "    int event_edge;",
                "    while (status == 0) begin",
                f"      @({port.name});",
                "      if (status == 0) begin",
                f"        event_edge = {port.name} ? EDGE_RISING : EDGE_FALLING;",
                "        if ((status == 0) &&",
                f"            (((event_edge == EDGE_RISING) && ((edge_interest[{constant}] & 1) != 0)) ||",
                f"             ((event_edge == EDGE_FALLING) && ((edge_interest[{constant}] & 2) != 0)))) begin",
                f"          run_step(PHASE_EDGE, {constant}, event_edge, requests);",
                "          service_requests(requests);",
                "        end",
                "      end",
                "    end",
                "  endtask",
                "",
            ]
        )

    lines.append("  initial begin")
    for clock in clocks:
        if clock_source(clock) != "generated":
            continue
        lines.append(f"    {clock['port']} = 1'b0;")
    for port in ports:
        if port.direction == "input" and port.name not in generated_clocks:
            initial_value = "'{default: '0}" if port.unpacked else "'0"
            lines.append(f"    {port.name} = {initial_value};")
    lines.extend(
        [
            "    sim_cycles = 0;",
            "    timer_generation = 0;",
            "    timer_deadline = NO_TIMER;",
            "    timer_owner_target = NO_TIMER;",
            f"    iterations = {default_iterations};",
            "    status = 0;",
            "    track_falling_edges = 1'b0;",
            f'    void\'($value$plusargs("{iteration_plusarg}=%d", iterations));',
            "",
            "    for (int i = 0; i < INPUT_WORD_COUNT; i++) begin",
            "      in_words[i] = '0;",
            "    end",
            "    for (int i = 0; i < OUTPUT_WORD_COUNT; i++) begin",
            "      out_words[i] = '0;",
            "    end",
            "    for (int i = 0; i < SIGNAL_COUNT; i++) begin",
            "      edge_interest[i] = '0;",
            "    end",
            "",
            f"    {init_function}(iterations, TIMEPRECISION_FS);",
            "    run_step(PHASE_INIT, NO_SIGNAL, EDGE_RISING, initial_requests);",
            "    service_requests(initial_requests);",
        ]
    )
    lines.extend(["    fork", "      timer_owner();"])
    for index, clock in enumerate(clocks):
        if clock_source(clock) == "generated":
            lines.append(f"      drive_clock_{index}();")
        else:
            lines.append(f"      observe_clock_{index}();")
    for index, _ in enumerate(edge_observers):
        lines.append(f"      observe_signal_{index}();")
    lines.append("    join_none")
    lines.extend(
        [
            "",
            "    wait (status != 0);",
            "    timer_generation++;",
            "    if (status < 0) begin",
            f'      $fatal(1, "{manifest["module"]} DPI testbench failed");',
            "    end",
            "    $finish;",
            "  end",
            "",
        ]
    )

    if parameters:
        lines.append(f"  {manifest['module']} #(")
        items = list(parameters)
        for index, name in enumerate(items):
            comma = "," if index + 1 < len(items) else ""
            lines.append(f"      .{name}({name}){comma}")
        lines.append("  ) i_dut (")
    else:
        lines.append(f"  {manifest['module']} i_dut (")
    for index, port in enumerate(ports):
        comma = "," if index + 1 < len(ports) else ""
        lines.append(f"      .{port.name}({port.name}){comma}")
    lines.extend(["  );", ""])
    lines.extend(sv_internal_export_functions(internals, manifest))
    lines.extend([f"endmodule : {manifest['top_module']}", ""])
    return "\n".join(lines)


def output_paths(manifest: dict[str, Any], base_dir: Path) -> dict[str, Path]:
    outputs = manifest.get("outputs", {})
    required = ("cpp_dut", "cpp_binding", "sv_wrapper", "cpp_include")
    for key in required:
        if key not in outputs:
            raise CodegenError(f"manifest outputs is missing required key {key!r}")
    return {
        key: (base_dir / outputs[key]).resolve()
        for key in ("cpp_dut", "cpp_binding", "sv_wrapper")
    }


def write_or_check(outputs: dict[Path, str], check: bool) -> None:
    stale: list[Path] = []
    for path, content in outputs.items():
        if check:
            try:
                current = path.read_text()
            except OSError:
                stale.append(path)
                continue
            if current != content:
                stale.append(path)
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        if path.exists() and path.read_text() == content:
            continue
        path.write_text(content)

    if stale:
        joined = "\n  ".join(str(path) for path in stale)
        raise CodegenError(f"generated files are stale:\n  {joined}")


def compare_designs(
    primary: DesignIR,
    comparison: DesignIR,
    primary_name: str,
    comparison_name: str,
) -> None:
    if primary.transport_signature() == comparison.transport_signature():
        return

    def format_port(signature: tuple) -> str:
        name, direction, width, signed, four_state, dimensions = signature
        unpacked = "".join(f"[{left}:{right}]" for left, right in dimensions)
        return (
            f"  {direction} {name}[{width}]{unpacked} "
            f"signed={signed} four_state={four_state}"
        )

    primary_ports = "\n".join(
        format_port(signature) for signature in primary.transport_signature()
    )
    comparison_ports = "\n".join(
        format_port(signature) for signature in comparison.transport_signature()
    )
    raise CodegenError(
        "elaboration frontends disagree on the DUT port contract:\n"
        f"{primary_name}:\n{primary_ports}\n"
        f"{comparison_name}:\n{comparison_ports}"
    )


def generate(
    manifest_path: Path,
    check: bool = False,
    frontend: str | None = None,
    compare_frontend: str | None = None,
) -> list[Path]:
    manifest_path = manifest_path.resolve()
    manifest = load_manifest(manifest_path)
    base_dir = manifest_path.parent
    primary_name = frontend_name(manifest, frontend)
    design = elaborate_design(manifest, base_dir, frontend)
    if compare_frontend is not None:
        comparison_manifest = manifest
        if compare_frontend == "verilator_json" and manifest.get("internals"):
            comparison_manifest = {**manifest, "internals": []}
        comparison = elaborate_design(
            comparison_manifest, base_dir, compare_frontend
        )
        compare_designs(design, comparison, primary_name, compare_frontend)

    discovered_ports = list(design.ports)
    internals = list(design.internals)
    validate_transport_ports(discovered_ports)
    validate_internals(internals)
    ports = map_ports(discovered_ports, manifest)
    root = build_tree([*ports, *internals])

    validate_clock_ports(manifest, ports)

    paths = output_paths(manifest, base_dir)
    source = manifest_path.name
    generated = {
        paths["cpp_dut"]: render_cpp_dut(
            ports, internals, root, manifest, source
        ),
        paths["cpp_binding"]: render_cpp_binding(
            ports, internals, root, manifest, source
        ),
        paths["sv_wrapper"]: render_sv(ports, internals, manifest, source),
    }
    write_or_check(generated, check)
    return list(generated)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument(
        "--check", action="store_true", help="fail when checked-in outputs are stale"
    )
    parser.add_argument(
        "--frontend",
        choices=("slang", "verilator_json"),
        help="override the manifest's elaboration frontend",
    )
    parser.add_argument(
        "--compare-frontend",
        choices=("slang", "verilator_json"),
        help="also elaborate with this frontend and compare the port contract",
    )
    args = parser.parse_args()
    try:
        paths = generate(
            args.manifest,
            check=args.check,
            frontend=args.frontend,
            compare_frontend=args.compare_frontend,
        )
    except CodegenError as error:
        print(f"cpptb-codegen: {error}", file=sys.stderr)
        return 1
    verb = "checked" if args.check else "generated"
    for path in paths:
        print(f"{verb} {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
