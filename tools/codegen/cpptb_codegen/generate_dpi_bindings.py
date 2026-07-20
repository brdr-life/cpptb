#!/usr/bin/env python3
"""Generate a typed C++ DUT binding and a batched SystemVerilog DPI wrapper."""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import OrderedDict
from dataclasses import dataclass, field, replace
from itertools import product
from pathlib import Path
from typing import Any

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from cpptb_codegen.design_ir import (
    CodegenError,
    DesignIR,
    HierarchyCatalog,
    HierarchyParameter,
    HierarchyScope,
    HierarchySignal,
    Internal,
    InterfaceConstructorPort,
    InterfacePort,
    PackedEnumType,
    PackedField,
    PackedIntegralType,
    PackedStructType,
    PackedType,
    PackedUnionType,
    Port,
)
from cpptb_codegen.frontends import elaborate_design, frontend_name
from cpptb_codegen.frontends.verilator_json import (
    discover_ports,
    parse_width,
    verilator_ast,
    walk_objects,
)


CLOCK_SOURCES = frozenset({"generated", "registered", "testbench", "dut"})
TIME_UNIT_FEMTOSECONDS = {
    "fs": 1,
    "ps": 1_000,
    "ns": 1_000_000,
    "us": 1_000_000_000,
    "ms": 1_000_000_000_000,
    "s": 1_000_000_000_000_000,
}

PORT_TRANSPORTS = frozenset({"packed", "on_demand"})
HIERARCHY_OPERATIONS = frozenset(
    {
        "get",
        "deposit",
        "force",
        "release",
        "rising_edge",
        "falling_edge",
        "any_edge",
        "get_logic",
        "deposit_logic",
        "force_logic",
    }
)


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


def format_time_literal(femtoseconds: int) -> str:
    for unit, multiplier in reversed(tuple(TIME_UNIT_FEMTOSECONDS.items())):
        if femtoseconds % multiplier == 0:
            return f"{femtoseconds // multiplier}{unit}"
    raise CodegenError("time value cannot be represented in femtoseconds")


def parse_clock_argument(value: str, *, primary: bool = True) -> dict[str, Any]:
    try:
        port, timing = value.split("=", 1)
    except ValueError as error:
        raise CodegenError(
            f"clock {value!r} must use PORT=PERIOD or "
            "PORT=PERIOD@PHASE syntax"
        ) from error
    period, separator, phase = timing.partition("@")
    validate_identifier(port, "clock port")
    period_fs = time_literal_femtoseconds(period, f"clock {port!r} period")
    if period_fs % 2 != 0:
        raise CodegenError(
            f"clock {port!r} period must be divisible into two whole "
            "femtosecond half-periods"
        )
    clock = {
        "port": port,
        "source": "generated",
        "half_period": format_time_literal(period_fs // 2),
        "primary": primary,
    }
    if separator:
        phase_fs = time_literal_femtoseconds(phase, f"clock {port!r} phase")
        clock["phase"] = format_time_literal(phase_fs)
    return clock


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


def source_manifest(
    sources: list[Path],
    top: str,
    *,
    output_dir: Path,
    clocks: list[dict[str, Any]] | None = None,
    edge_observers: list[str] | None = None,
    target: str | None = None,
    namespace: str | None = None,
    root_type: str = "Dut",
    frontend: str = "slang",
    dynamic_clocks: bool = True,
    include_dirs: list[Path] | None = None,
    defines: list[str] | None = None,
    parameters: dict[str, str | int] | None = None,
) -> dict[str, Any]:
    if not sources:
        raise CodegenError("source-driven generation requires at least one source")
    validate_identifier(top, "top module")
    target = target or top
    validate_identifier(target, "target")
    validate_identifier(root_type, "root type")
    namespace = namespace or f"cpptb::generated::{target}"
    parts = namespace.split("::")
    if not parts or any(not part for part in parts):
        raise CodegenError(f"namespace {namespace!r} is not valid")
    for part in parts:
        validate_identifier(part, "namespace component")

    stem = target
    compatibility_root = f"{pascal_case(top)}Dut"
    run_prefix = f"cpptb_{target}_dpi"
    manifest: dict[str, Any] = {
        "schema_version": 1,
        "target": target,
        "module": top,
        "top_module": f"dpi_{target}",
        "namespace": namespace,
        "root_type": root_type,
        "frontend": frontend,
        "frontend_options": {
            "slang": {"standard": "1800-2023"},
            "verilator_json": {"args": ["-Wno-TIMESCALEMOD"]},
        },
        "codegen": {"static_binding": True},
        "sources": [str(source.resolve()) for source in sources],
        "include_dirs": [str(path.resolve()) for path in include_dirs or []],
        "defines": defines or [],
        "parameters": parameters or {},
        "clocks": clocks or [],
        "edge_observers": edge_observers or [],
        "auto_edge_observers": True,
        "run": {
            "init_function": f"{run_prefix}_init",
            "step_function": f"{run_prefix}_step",
            "pull_outputs_function": f"{run_prefix}_pull_outputs",
            "next_deadline_function": f"{run_prefix}_next_timer_deadline",
            "edge_interest_function": f"{run_prefix}_edge_interest",
            "clock_config_function": f"{run_prefix}_clock_config",
            "phase_dispatch_function": f"{run_prefix}_phase_dispatch",
            "dynamic_clocks": dynamic_clocks,
            "iteration_plusarg": None,
            "default_iterations": 1,
            "timeprecision": "1ps",
            "timeout_cycles": 1_000_000,
        },
        "outputs": {
            "cpp_dut": str((output_dir / f"{stem}_dut.hpp").resolve()),
            "cpp_binding": str(
                (output_dir / f"{stem}_binding.hpp").resolve()
            ),
            "cpp_adapter": str(
                (output_dir / f"dpi_{stem}.cpp").resolve()
            ),
            "cpp_clock_discovery": str(
                (output_dir / f"discover_{stem}_clocks.cpp").resolve()
            ),
            "sv_wrapper": str((output_dir / f"dpi_{stem}.sv").resolve()),
            "cpp_include": f"{stem}_dut.hpp",
            "cpp_binding_include": f"{stem}_binding.hpp",
            "cpp_public_include": str((output_dir / "dut.hpp").resolve()),
        },
    }
    if compatibility_root != root_type:
        manifest["compatibility_root_type"] = compatibility_root
    return manifest


def load_discovered_clocks(path: Path) -> list[dict[str, Any]]:
    try:
        data = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise CodegenError(f"cannot read discovered clocks {path}: {error}") from error
    if not isinstance(data, dict) or data.get("schema_version") != 1:
        raise CodegenError("discovered clock file schema_version must be 1")
    entries = data.get("clocks")
    if not isinstance(entries, list):
        raise CodegenError("discovered clock file must contain a clocks list")

    clocks: list[dict[str, Any]] = []
    seen: set[int | str] = set()
    primary_count = 0
    for entry in entries:
        if not isinstance(entry, dict):
            raise CodegenError("each discovered clock must be an object")
        port = entry.get("port")
        if not isinstance(port, str) or not port:
            raise CodegenError("each discovered clock must name a port")
        signal_id_value = entry.get("signal_id")
        if signal_id_value is not None and (
            not isinstance(signal_id_value, int)
            or isinstance(signal_id_value, bool)
            or signal_id_value < 0
        ):
            raise CodegenError(
                f"discovered clock {port!r} signal_id must be nonnegative"
            )
        identity: int | str = (
            signal_id_value if signal_id_value is not None else port
        )
        if identity in seen:
            raise CodegenError(
                f"discovered clock {port!r} signal {identity!r} appears more "
                "than once"
            )
        seen.add(identity)
        period_fs = entry.get("period_fs")
        phase_fs = entry.get("phase_fs", 0)
        initial_value = entry.get("initial_value", 0)
        primary = entry.get("primary", False)
        if not isinstance(period_fs, int) or isinstance(period_fs, bool) or period_fs <= 0:
            raise CodegenError(f"discovered clock {port!r} period_fs must be positive")
        if period_fs % 2 != 0:
            raise CodegenError(f"discovered clock {port!r} period must be even")
        if not isinstance(phase_fs, int) or isinstance(phase_fs, bool) or phase_fs < 0:
            raise CodegenError(f"discovered clock {port!r} phase_fs cannot be negative")
        if not isinstance(primary, bool):
            raise CodegenError(f"discovered clock {port!r} primary must be boolean")
        if not isinstance(initial_value, int) or isinstance(initial_value, bool) or initial_value not in {0, 1}:
            raise CodegenError(
                f"discovered clock {port!r} initial_value must be zero or one"
            )
        clock = {
            "port": port,
            "source": "registered",
            "half_period": format_time_literal(period_fs // 2),
            "period_fs": period_fs,
            "phase_fs": phase_fs,
            "initial_value": initial_value,
            "primary": primary,
        }
        if signal_id_value is not None:
            clock["signal_id"] = signal_id_value
        if phase_fs:
            clock["phase"] = format_time_literal(phase_fs)
        clocks.append(clock)
        primary_count += int(primary)
    if primary_count > 1:
        raise CodegenError("only one discovered clock may be primary")
    if clocks and primary_count == 0:
        clocks[0]["primary"] = True
    return clocks


def load_access_plan(
    path: Path,
) -> tuple[list[dict[str, str]], list[int]]:
    try:
        data = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise CodegenError(
            f"cannot read hierarchy access plan {path}: {error}"
        ) from error
    if not isinstance(data, dict) or data.get("schema_version") != 1:
        raise CodegenError("hierarchy access plan schema_version must be 1")
    entries = data.get("accesses")
    if not isinstance(entries, list):
        raise CodegenError("hierarchy access plan must contain an accesses list")

    accesses: set[tuple[str, str]] = set()
    for entry in entries:
        if not isinstance(entry, dict):
            raise CodegenError("each hierarchy access must be an object")
        path_value = entry.get("path")
        operation = entry.get("operation")
        if not isinstance(path_value, str) or not path_value:
            raise CodegenError("each hierarchy access must name a path")
        if operation not in HIERARCHY_OPERATIONS:
            raise CodegenError(
                f"hierarchy access {path_value!r} has unsupported operation "
                f"{operation!r}"
            )
        accesses.add((path_value, operation))
    hierarchy_accesses = [
        {"path": path_value, "operation": operation}
        for path_value, operation in sorted(accesses)
    ]
    port_edges = data.get("port_edges", [])
    if not isinstance(port_edges, list) or not all(
        isinstance(signal_id, int)
        and not isinstance(signal_id, bool)
        and signal_id >= 0
        for signal_id in port_edges
    ):
        raise CodegenError(
            "hierarchy access plan port_edges must contain nonnegative integers"
        )
    return hierarchy_accesses, sorted(set(port_edges))


def load_hierarchy_access_plan(path: Path) -> list[dict[str, str]]:
    return load_access_plan(path)[0]


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


def registered_clock_configs(manifest: dict[str, Any]) -> list[dict[str, Any]]:
    return [
        clock
        for clock in clock_configs(manifest)
        if clock_source(clock) == "registered"
    ]


def clock_signal_id_expression(clock: dict[str, Any]) -> str:
    discovered = clock.get("signal_id")
    if isinstance(discovered, int) and not isinstance(discovered, bool):
        return str(discovered)
    return signal_id(clock["port"])


def same_clock(left: dict[str, Any], right: dict[str, Any]) -> bool:
    return clock_signal_id_expression(left) == clock_signal_id_expression(right)


def _unflatten_port_index(
    port: Port, linear_index: int
) -> tuple[int, ...]:
    indices = [0] * len(port.unpacked)
    remainder = linear_index
    for rank in reversed(range(len(port.unpacked))):
        dimension = port.unpacked[rank]
        indices[rank] = dimension.low + remainder % dimension.size
        remainder //= dimension.size
    return tuple(indices)


def resolve_clock_port(
    clock: dict[str, Any], ports: list[Port]
) -> tuple[Port, tuple[int, ...]]:
    discovered = clock.get("signal_id")
    if isinstance(discovered, int) and not isinstance(discovered, bool):
        offsets = signal_word_offsets(ports)
        for port, begin, end in zip(ports, offsets, offsets[1:]):
            if not (begin <= discovered < end):
                continue
            if port.width != 1:
                raise CodegenError(
                    f"clock signal ID {discovered} selects word storage inside "
                    f"non-scalar port {port.name!r}"
                )
            linear_index = discovered - begin
            return port, _unflatten_port_index(port, linear_index)
        raise CodegenError(
            f"clock signal ID {discovered} is outside the generated DUT ports"
        )

    name = clock.get("port")
    port = next((candidate for candidate in ports if candidate.name == name), None)
    if port is None:
        raise CodegenError(f"clock port {name!r} was not found")
    if port.unpacked:
        raise CodegenError(
            f"clock port {name!r} is an array; register a named element from "
            "the generated Dut so discovery records its signal ID"
        )
    return port, ()


def clock_period_femtoseconds(clock: dict[str, Any]) -> int:
    period_fs = clock.get("period_fs")
    if isinstance(period_fs, int) and not isinstance(period_fs, bool):
        return period_fs
    return 2 * time_literal_femtoseconds(
        clock["half_period"], f"clock {clock.get('port')!r} half-period"
    )


def clock_phase_femtoseconds(clock: dict[str, Any]) -> int:
    phase_fs = clock.get("phase_fs")
    if isinstance(phase_fs, int) and not isinstance(phase_fs, bool):
        return phase_fs
    phase = clock.get("phase")
    return 0 if phase is None else time_literal_femtoseconds(
        phase, f"clock {clock.get('port')!r} phase"
    )


def validate_clock_ports(manifest: dict[str, Any], ports: list[Port]) -> None:
    clocks = clock_configs(manifest)
    seen_clocks: set[int | str] = set()
    primary_count = 0

    for clock in clocks:
        clock_name = clock.get("port")
        if not isinstance(clock_name, str) or not clock_name:
            raise CodegenError("each clock must name a port")
        clock_identity = clock_signal_id_expression(clock)
        if clock_identity in seen_clocks:
            raise CodegenError(
                f"clock signal {clock_identity!r} is configured more than once"
            )
        seen_clocks.add(clock_identity)

        source = clock_source(clock)
        if source not in CLOCK_SOURCES:
            choices = ", ".join(sorted(CLOCK_SOURCES))
            raise CodegenError(
                f"clock port {clock_name!r} has invalid source {source!r}; "
                f"expected one of {choices}"
            )

        port, _ = resolve_clock_port(clock, ports)
        if port.width != 1:
            raise CodegenError(
                f"clock port {clock_name!r} must name one single-bit DUT port"
            )
        expected_direction = "output" if source == "dut" else "input"
        if port.direction != expected_direction:
            raise CodegenError(
                f"{source} clock port {clock_name!r} must be a DUT "
                f"{expected_direction}"
            )

        if source not in {"generated", "registered"} and any(
            key in clock for key in ("half_period", "phase")
        ):
            raise CodegenError(
                f"{source} clock port {clock_name!r} cannot define "
                "half_period or phase"
            )
        if source == "registered" and "half_period" not in clock:
            raise CodegenError(
                f"{source} clock port {clock_name!r} must define half_period"
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
        replace(
            port,
            cpp_path=(
                port.cpp_path
                if port.interface_name is not None
                else path_for_port(port.name, manifest)
            ),
        )
        for port in ports
    ]
    paths: dict[tuple[str, ...], str] = {}
    for port in mapped:
        for component in port.cpp_path:
            validate_identifier(
                component,
                f"C++ path component for port {port.name!r}",
            )
        previous = paths.setdefault(port.cpp_path, port.name)
        if previous != port.name:
            raise CodegenError(
                f"ports {previous!r} and {port.name!r} map to the same C++ path "
                f"{'.'.join(port.cpp_path)!r}"
            )
    return mapped


def apply_port_transport(design: DesignIR, manifest: dict[str, Any]) -> DesignIR:
    configured = manifest.get("port_transport", {})
    if not isinstance(configured, dict):
        raise CodegenError("manifest port_transport must be an object")
    if not all(isinstance(name, str) and name for name in configured):
        raise CodegenError("manifest port_transport keys must be port names")

    by_name = {port.name: port for port in design.ports}
    clock_names = {
        clock.get("port")
        for clock in clock_configs(manifest)
        if "port" in clock
    }
    configured_observers = manifest.get("edge_observers", [])
    edge_observer_names = (
        {name for name in configured_observers if isinstance(name, str)}
        if isinstance(configured_observers, list)
        else set()
    )
    for name, transport in configured.items():
        if name not in by_name:
            raise CodegenError(f"port_transport port {name!r} was not found")
        if not isinstance(transport, str) or transport not in PORT_TRANSPORTS:
            choices = ", ".join(sorted(PORT_TRANSPORTS))
            raise CodegenError(
                f"port {name!r} has invalid transport {transport!r}; "
                f"expected one of {choices}"
            )
        if transport == "on_demand" and name in clock_names:
            raise CodegenError(
                f"configured clock port {name!r} cannot use on_demand transport"
            )
        if transport == "on_demand" and name in edge_observer_names:
            raise CodegenError(
                f"edge observer port {name!r} cannot use on_demand transport"
            )

    return replace(
        design,
        ports=tuple(
            replace(port, transport=configured.get(port.name, "packed"))
            for port in design.ports
        ),
    )


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
        if port.direction not in {"input", "output", "inout"}:
            raise CodegenError(
                f"port {port.name!r} has direction {port.direction!r}; "
                "the current DPI transport supports input, output, and inout "
                "ports"
            )
        if port.width > 32 and port.four_state and port.direction != "inout":
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


def validate_hierarchy_accesses(
    hierarchy: HierarchyCatalog, accesses: list[dict[str, str]]
) -> None:
    signals = {signal.hdl_path: signal for signal in hierarchy.signals}
    for access in accesses:
        path = access["path"]
        operation = access["operation"]
        signal = signals.get(path)
        if signal is None:
            raise CodegenError(
                f"hierarchy access path {path!r} was not found in the "
                "elaborated DUT"
            )
        if operation in {"deposit", "deposit_logic"} and not signal.depositable:
            raise CodegenError(
                f"hierarchy net {path!r} does not support deposit; use force"
            )
        if operation.endswith("_logic") and not signal.four_state:
            raise CodegenError(
                f"hierarchy signal {path!r} is two-state and does not need "
                f"{operation}"
            )
        if operation.endswith("edge") and (
            signal.width != 1 or signal.unpacked
        ):
            raise CodegenError(
                f"hierarchy edge access {path!r} requires a scalar one-bit signal"
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


def collect_packed_cpp_types(
    ports: list[Port], hierarchy: HierarchyCatalog | None = None
) -> PackedCppRegistry:
    registry = PackedCppRegistry()
    for port in ports:
        if port.packed_type is None:
            continue
        registry.register(port.packed_type, port.cpp_path or (port.name,))
    if hierarchy is not None:
        for signal in hierarchy.signals:
            if signal.packed_type is None or isinstance(
                signal.packed_type, PackedUnionType
            ):
                continue
            registry.register(signal.packed_type, signal.cpp_path)
    return registry


def signal_id(port_name: str) -> str:
    return "kSignal" + pascal_case(port_name)


def internal_export_name(
    manifest: dict[str, Any], index: int, operation: str
) -> str:
    return f"{manifest['top_module']}_internal_{index}_{operation}"


def hierarchy_export_name(
    manifest: dict[str, Any], index: int, operation: str
) -> str:
    return f"{manifest['top_module']}_hierarchy_{index}_{operation}"


def port_export_name(
    manifest: dict[str, Any], index: int, operation: str
) -> str:
    return f"{manifest['top_module']}_port_{index}_{operation}"


def inout_export_name(
    manifest: dict[str, Any], index: int, operation: str
) -> str:
    return f"{manifest['top_module']}_inout_{index}_{operation}"


def cpp_inout_helpers(ports: list[Port], manifest: dict[str, Any]) -> list[str]:
    lines: list[str] = []
    for index, port in enumerate(ports):
        if port.direction != "inout":
            continue
        value_type = f"coro::PackedSignalValue<{port.width}>"
        drive_name = inout_export_name(manifest, index, "drive")
        high_z_name = inout_export_name(manifest, index, "high_z")
        if port.width <= 32:
            exported_value = "unsigned int value"
        elif port.width <= 64:
            exported_value = "unsigned long long value"
        else:
            exported_value = "const svBitVecVal* value"
        lines.extend(
            [
                f'extern "C" void {drive_name}(int index, {exported_value});',
                f'extern "C" void {high_z_name}(int index);',
                "",
                f"inline void cpptb_inout_{index}_drive(",
                f"    std::int32_t index, {value_type} value) {{",
            ]
        )
        if port.width <= 64:
            lines.append(f"    {drive_name}(index, value);")
        else:
            lines.append(
                f"    {drive_name}(index, reinterpret_cast<const "
                "svBitVecVal*>(value.words().data()));"
            )
        lines.extend(
            [
                "}",
                "",
                f"inline void cpptb_inout_{index}_high_z(std::int32_t index) {{",
                f"    {high_z_name}(index);",
                "}",
                "",
                "template <typename Observed>",
                f"inline auto cpptb_make_inout_{index}(Observed observed) {{",
            ]
        )
        callback_names = (
            f"cpptb_inout_{index}_drive, cpptb_inout_{index}_high_z"
        )
        if port.unpacked:
            dimensions = ", ".join(
                f"coro::ArrayDimension<{dimension.left}, {dimension.right}>"
                for dimension in port.unpacked
            )
            lines.append(
                f"    return cpptb::InoutArray<{port.width}, Observed, "
                f"{callback_names}, 0, {dimensions}>{{observed, 0}};"
            )
        else:
            lines.append(
                f"    return cpptb::InoutRef<{port.width}, Observed, "
                f"{callback_names}>{{observed, 0}};"
            )
        lines.extend(["}", ""])
    return lines


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


def packed_ports(ports: list[Port]) -> list[Port]:
    return [port for port in ports if port.transport == "packed"]


def on_demand_ports(ports: list[Port]) -> list[Port]:
    return [port for port in ports if port.transport == "on_demand"]


def port_element_count(port: Port) -> int:
    count = 1
    for dimension in port.unpacked:
        count *= dimension.size
    return count


def port_linear_index(port: Port, indices: tuple[int, ...]) -> int:
    if len(indices) != len(port.unpacked):
        raise CodegenError(
            f"port {port.name!r} expected {len(port.unpacked)} indices, "
            f"got {len(indices)}"
        )
    linear = 0
    for dimension, index in zip(port.unpacked, indices):
        if index < dimension.low or index > dimension.high:
            raise CodegenError(
                f"port {port.name!r} clock index {index} is outside "
                f"[{dimension.left}:{dimension.right}]"
            )
        linear = linear * dimension.size + index - dimension.low
    return linear


def clock_owned_element_indices(
    ports: list[Port],
    manifest: dict[str, Any],
    sources: set[str],
) -> dict[str, set[int]]:
    owned: dict[str, set[int]] = {}
    for clock in clock_configs(manifest):
        if clock_source(clock) not in sources:
            continue
        port, indices = resolve_clock_port(clock, ports)
        owned.setdefault(port.name, set()).add(
            port_linear_index(port, indices)
        )
    return owned


def driven_port_names(ports: list[Port], manifest: dict[str, Any]) -> set[str]:
    clock_owned = clock_owned_element_indices(
        ports, manifest, {"generated", "registered"}
    )
    return {
        port.name
        for port in ports
        if port.direction == "input"
        and len(clock_owned.get(port.name, set())) < port_element_count(port)
    }


def writable_port_names(ports: list[Port], manifest: dict[str, Any]) -> set[str]:
    generated_clock_owned = clock_owned_element_indices(
        ports, manifest, {"generated"}
    )
    return {
        port.name
        for port in ports
        if port.direction == "input"
        and len(generated_clock_owned.get(port.name, set()))
        < port_element_count(port)
    }


def observed_port_names(
    ports: list[Port], manifest: dict[str, Any]
) -> set[str]:
    clock_owned = clock_owned_element_indices(
        ports, manifest, {"generated", "registered"}
    )
    return {
        port.name
        for port in ports
        if port.direction != "input" or bool(clock_owned.get(port.name))
    }


def static_binding_enabled(manifest: dict[str, Any]) -> bool:
    codegen = manifest.get("codegen", {})
    if not isinstance(codegen, dict):
        raise CodegenError("manifest codegen must be an object")
    enabled = codegen.get("static_binding", False)
    if not isinstance(enabled, bool):
        raise CodegenError("manifest codegen.static_binding must be a boolean")
    return enabled


def static_packed_transport_offsets(
    ports: list[Port], manifest: dict[str, Any]
) -> dict[str, int]:
    driven_names = driven_port_names(ports, manifest)
    observed_names = observed_port_names(ports, manifest)
    packed_observed = [
        port
        for port in ports
        if port.transport == "packed" and port.name in observed_names
    ]
    packed_driven = [
        port
        for port in ports
        if port.transport == "packed" and port.name in driven_names
    ]
    driven_offsets = directional_transport_offsets(packed_driven)
    observed_offsets = directional_transport_offsets(packed_observed)
    return {
        port.name: (
            driven_offsets[port.name]
            if port.name in driven_offsets
            else observed_offsets[port.name]
        )
        for port in [*packed_observed, *packed_driven]
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
        if port.direction not in {"output", "inout"}:
            raise CodegenError(
                f"edge observer port {name!r} must be a DUT output or inout"
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
    if manifest.get("auto_edge_observers", False):
        configured_names = {port.name for port in observers}
        discovered_ids = manifest.get("edge_observer_signal_ids")
        strict_discovery = discovered_ids is not None
        if discovered_ids is None:
            candidates = ports
        else:
            offsets = signal_word_offsets(ports)
            by_signal_id = {
                offset: port for port, offset in zip(ports, offsets)
            }
            unknown = sorted(set(discovered_ids) - set(by_signal_id))
            if unknown:
                raise CodegenError(
                    "discovered port-edge signal IDs were not found in the "
                    f"DUT: {unknown}"
                )
            candidates = [by_signal_id[signal_id] for signal_id in discovered_ids]
        for port in candidates:
            if port.name in clock_names or port.direction == "input":
                continue
            if (
                port.direction not in {"output", "inout"}
                or port.unpacked
                or port.width != 1
            ):
                if not strict_discovery:
                    continue
                raise CodegenError(
                    f"edge wait on DUT port {port.name!r} requires a scalar "
                    "one-bit output"
                )
            if port.name not in configured_names:
                observers.append(port)
                configured_names.add(port.name)
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


def cpp_inout_type(port: Port, observed_type: str, index: int) -> str:
    callbacks = f"cpptb_inout_{index}_drive, cpptb_inout_{index}_high_z"
    if not port.unpacked:
        return (
            f"cpptb::InoutRef<{port.width}, {observed_type}, {callbacks}>"
        )
    dimensions = ", ".join(
        f"coro::ArrayDimension<{dimension.left}, {dimension.right}>"
        for dimension in port.unpacked
    )
    return (
        f"cpptb::InoutArray<{port.width}, {observed_type}, {callbacks}, "
        f"0, {dimensions}>"
    )


def cpp_static_signal_type(
    port: Port,
    writable_names: set[str],
    driven_names: set[str],
    packed_transport_offsets: dict[str, int],
) -> str:
    writable = "true" if port.name in writable_names else "false"
    driven = "true" if port.name in driven_names else "false"
    transport = "Packed" if port.transport == "packed" else "OnDemand"
    template_values = f"{port.width}, {writable}, {driven}, {signal_id(port.name)}"
    if port.transport == "packed":
        template_values += f", {packed_transport_offsets[port.name]}"
    if port.unpacked:
        dimensions = ", ".join(
            f"coro::ArrayDimension<{dimension.left}, {dimension.right}>"
            for dimension in port.unpacked
        )
        return (
            f"cpptb::dpi::Static{transport}FixedArray<{template_values}"
            f", 0, {dimensions}>"
        )
    return f"cpptb::dpi::Static{transport}Signal<{template_values}>"


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


def node_signature(
    node: TreeNode,
    manifest: dict[str, Any],
    writable_names: set[str],
) -> tuple[Any, ...]:
    signature = []
    for name, child in node.children.items():
        if isinstance(child, Port):
            writable = child.name in writable_names
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


def collect_structs(
    root: TreeNode,
    manifest: dict[str, Any],
    writable_names: set[str],
) -> list[TreeNode]:
    ordered: list[TreeNode] = []
    by_type: dict[str, tuple[Any, ...]] = {}

    def visit(node: TreeNode) -> None:
        for child in node.children.values():
            if isinstance(child, TreeNode):
                visit(child)
        type_name = node_type(node, manifest)
        signature = node_signature(node, manifest, writable_names)
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
    return f"// Generated by cpptb-codegen from {source}.\n// Do not edit by hand.\n"


def render_cpp_public_include(manifest: dict[str, Any], source: str) -> str:
    return "\n".join(
        [
            generated_banner(source).rstrip(),
            "#pragma once",
            "",
            f'#include "{manifest["outputs"]["cpp_include"]}"',
            "",
            "namespace cpptb {",
            f"using Dut = ::{manifest['namespace']}::{manifest['root_type']};",
            "}  // namespace cpptb",
            "",
        ]
    )


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
    lines.extend(
        [
            "};",
            "",
            "[[nodiscard]] inline constexpr std::string_view "
            f"cpptb_diagnostic_name({entry.base_name} value) {{",
        ]
    )
    for item in data_type.values:
        name = cpp_identifier(item.name, pascal=True)
        lines.append(
            f"    if (value == {entry.base_name}::{name}) return \"{item.name}\";"
        )
    lines.extend(["    return {};", "}", "", f"class {value_name} {{", "public:"])
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


def hierarchy_scope_type(path: tuple[str, ...]) -> str:
    suffix = "".join(cpp_identifier(part, pascal=True) for part in path)
    return f"Hierarchy{suffix}Scope"


def hierarchy_scope_array_type(
    parent: tuple[str, ...], base_name: str
) -> str:
    suffix = "".join(
        cpp_identifier(part, pascal=True) for part in (*parent, base_name)
    )
    return f"Hierarchy{suffix}Array"


def hierarchy_signal_id(path: str) -> str:
    return "kHierarchy" + pascal_case(path)


def hierarchy_edge_signal_id(path: str) -> str:
    return "kHierarchyEdge" + pascal_case(path)


def hierarchy_edge_paths(accesses: list[dict[str, str]]) -> list[str]:
    return sorted(
        {
            access["path"]
            for access in accesses
            if access["operation"].endswith("edge")
        }
    )


def hierarchy_signal_cpp_type(
    signal: HierarchySignal,
    signal_index: int,
    packed_types: PackedCppRegistry,
) -> str:
    depositable = "true" if signal.depositable else "false"
    four_state = "true" if signal.four_state else "false"
    common = (
        f"HierarchyTransport, {signal_index}, \"{signal.hdl_path}\", "
        f"{signal.width}"
    )
    user_value = f"cpptb::probe::Value<{signal.width}>"
    if isinstance(signal.packed_type, (PackedEnumType, PackedStructType)):
        user_value = packed_types.lookup(signal.packed_type).value_name
    if not signal.unpacked:
        return (
            f"cpptb::hierarchy::Signal<{common}, {depositable}"
            f", {user_value}, {four_state}>"
        )
    if len(signal.unpacked) == 1:
        dimension = signal.unpacked[0]
        return (
            f"cpptb::hierarchy::Memory<{common}, {dimension.left}, "
            f"{dimension.right}, {depositable}, {user_value}, "
            f"{four_state}>"
        )
    dimensions = ", ".join(
        f"cpptb::hierarchy::Dimension<{dimension.left}, {dimension.right}>"
        for dimension in signal.unpacked
    )
    return (
        f"cpptb::hierarchy::MemoryND<{common}, {depositable}, "
        f"{four_state}, {user_value}, {dimensions}>"
    )


def selected_hierarchy_signal_cpp_type(
    signals: tuple[HierarchySignal, ...], packed_types: PackedCppRegistry
) -> str:
    signal = signals[0]
    depositable = "true" if signal.depositable else "false"
    four_state = "true" if signal.four_state else "false"
    access_paths = (
        "cpptb::hierarchy::AccessPathSet<"
        + ", ".join(json.dumps(item.hdl_path) for item in signals)
        + ">"
    )
    user_value = f"cpptb::probe::Value<{signal.width}>"
    if isinstance(signal.packed_type, (PackedEnumType, PackedStructType)):
        user_value = packed_types.lookup(signal.packed_type).value_name
    if not signal.unpacked:
        return (
            f"cpptb::hierarchy::SelectedSignal<HierarchyTransport, "
            f"{signal.width}, {depositable}, {user_value}, {four_state}, "
            f"{access_paths}>"
        )
    if len(signal.unpacked) == 1:
        dimension = signal.unpacked[0]
        return (
            f"cpptb::hierarchy::SelectedMemory<HierarchyTransport, "
            f"{signal.width}, {dimension.left}, {dimension.right}, "
            f"{depositable}, {user_value}, {four_state}, {access_paths}>"
        )
    dimensions = ", ".join(
        f"cpptb::hierarchy::Dimension<{dimension.left}, {dimension.right}>"
        for dimension in signal.unpacked
    )
    return (
        f"cpptb::hierarchy::SelectedMemoryND<HierarchyTransport, "
        f"{signal.width}, {depositable}, {four_state}, {user_value}, "
        f"{access_paths}, "
        f"{dimensions}>"
    )


def hierarchy_nodes(
    hierarchy: HierarchyCatalog,
) -> tuple[
    set[tuple[str, ...]],
    dict[tuple[str, ...], list[HierarchySignal]],
    dict[tuple[str, ...], list[HierarchyParameter]],
]:
    node_paths: set[tuple[str, ...]] = {()}
    signals: dict[tuple[str, ...], list[HierarchySignal]] = {}
    parameters: dict[tuple[str, ...], list[HierarchyParameter]] = {}

    def add_parents(path: tuple[str, ...]) -> None:
        for length in range(len(path) + 1):
            node_paths.add(path[:length])

    array_containers = {
        scope.cpp_path
        for scope in hierarchy.scopes
        if scope.symbol_kind in {"generate_array", "instance_array"}
    }
    for scope in hierarchy.scopes:
        if scope.cpp_path not in array_containers:
            add_parents(scope.cpp_path)
    for signal in hierarchy.signals:
        parent = signal.cpp_path[:-1]
        add_parents(parent)
        signals.setdefault(parent, []).append(signal)
    for parameter in hierarchy.parameters:
        parent = parameter.cpp_path[:-1]
        if parent in array_containers:
            continue
        add_parents(parent)
        parameters.setdefault(parent, []).append(parameter)
    return node_paths, signals, parameters


def render_cpp_hierarchy_transport(
    hierarchy: HierarchyCatalog,
    accesses: list[dict[str, str]],
    manifest: dict[str, Any],
) -> list[str]:
    if not hierarchy.signals:
        return []
    indices = {
        signal.hdl_path: index
        for index, signal in enumerate(hierarchy.signals)
    }
    signals = {signal.hdl_path: signal for signal in hierarchy.signals}
    selected: dict[str, list[HierarchySignal]] = {
        operation: [] for operation in HIERARCHY_OPERATIONS
    }
    for access in accesses:
        selected[access["operation"]].append(signals[access["path"]])
    selected_paths = {
        operation: {signal.hdl_path for signal in operation_signals}
        for operation, operation_signals in selected.items()
    }
    immediate_force_get_paths = (
        selected_paths["get"] & selected_paths["force"]
    )
    immediate_logic_force_get_paths = (
        selected_paths["get_logic"] & selected_paths["force_logic"]
    )

    lines = ['extern "C" {']
    for operation in ("get", "deposit", "force", "release"):
        for signal in selected[operation]:
            name = hierarchy_export_name(
                manifest, indices[signal.hdl_path], operation
            )
            if operation == "release":
                lines.append(f"void {name}(int index);")
                continue
            if signal.width <= 32:
                value_type = "unsigned int"
            elif signal.width <= 64:
                value_type = "unsigned long long"
            else:
                value_type = "unsigned int*"
            if operation == "get":
                if signal.width <= 64:
                    lines.append(f"{value_type} {name}(int index);")
                else:
                    lines.append(f"void {name}(int index, {value_type} value);")
            else:
                const_prefix = "const " if signal.width > 64 else ""
                lines.append(
                    f"void {name}(int index, {const_prefix}{value_type} value);"
                )
    for operation in ("get", "deposit"):
        for signal in selected[operation]:
            if len(signal.unpacked) != 1 or signal.width > 64:
                continue
            name = hierarchy_export_name(
                manifest, indices[signal.hdl_path], f"{operation}_block4"
            )
            const_prefix = "const " if operation == "deposit" else ""
            lines.append(
                f"void {name}(int first_index, int count, "
                f"{const_prefix}unsigned int* values);"
            )
    for operation in ("get_logic", "deposit_logic", "force_logic"):
        for signal in selected[operation]:
            name = hierarchy_export_name(
                manifest, indices[signal.hdl_path], operation
            )
            if operation == "get_logic":
                lines.append(
                    f"void {name}(int index, svLogicVecVal* value);"
                )
            else:
                lines.append(
                    f"void {name}(int index, const svLogicVecVal* value);"
                )
    lines.extend(['}  // extern "C"', ""])
    if immediate_force_get_paths or immediate_logic_force_get_paths:
        lines.extend(
            [
                "template <std::uint32_t Id, typename Value>",
                "struct HierarchyImmediateForceCache {",
                "    static constexpr std::uint64_t invalid_callback_epoch =",
                "        ~std::uint64_t{0};",
                "    inline static thread_local std::uint64_t callback_epoch =",
                "        invalid_callback_epoch;",
                "    inline static thread_local std::int32_t index = 0;",
                "    inline static thread_local Value value{};",
                "};",
                "",
            ]
        )
    lines.append("struct HierarchyTransport {")

    lines.extend(
        [
            "    template <std::size_t Width>",
            "    static cpptb::probe::Value<Width> get(std::uint32_t id,",
            "                                           std::int32_t index) {",
        ]
    )
    for width_group, condition in (
        (32, "Width <= 32"),
        (64, "Width > 32 && Width <= 64"),
        (0, "Width > 64"),
    ):
        group = [
            signal
            for signal in selected["get"]
            if (
                (width_group == 32 and signal.width <= 32)
                or (width_group == 64 and 32 < signal.width <= 64)
                or (width_group == 0 and signal.width > 64)
            )
        ]
        lines.append(f"        if constexpr ({condition}) {{")
        lines.append("            switch (id) {")
        for signal in group:
            index = indices[signal.hdl_path]
            name = hierarchy_export_name(manifest, index, "get")
            lines.append(f"                case {index}:")
            cached = signal.hdl_path in immediate_force_get_paths
            if cached:
                lines.extend(
                    [
                        "                {",
                        "                    using Cache =",
                        f"                        HierarchyImmediateForceCache<{index},",
                        "                            cpptb::probe::Value<Width>>;",
                        "                    if (Cache::callback_epoch ==",
                        "                            cpptb::probe::detail::current_callback_epoch() &&",
                        "                        Cache::index == index) {",
                        "                        return Cache::value;",
                        "                    }",
                    ]
                )
            if width_group:
                lines.append(
                    f"                    return static_cast<cpptb::probe::Value<Width>>("
                    f"{name}(index));"
                )
            else:
                lines.extend(
                    [
                        "                {",
                        "                    typename cpptb::Bits<Width>::word_array words{};",
                        f"                    {name}(index, words.data());",
                        "                    return cpptb::Bits<Width>::from_words(words);",
                        "                }",
                    ]
                )
            if cached:
                lines.append("                }")
        lines.extend(["                default: break;", "            }", "        }"])
    lines.extend(
        [
            '        fail("get", id);',
            "    }",
            "",
        ]
    )

    for operation in ("deposit", "force"):
        lines.extend(
            [
                "    template <std::size_t Width>",
                f"    static void {operation}(std::uint32_t id, std::int32_t index,",
                "                        cpptb::probe::Value<Width> value) {",
            ]
        )
        for width_group, condition in (
            (32, "Width <= 32"),
            (64, "Width > 32 && Width <= 64"),
            (0, "Width > 64"),
        ):
            group = [
                signal
                for signal in selected[operation]
                if (
                    (width_group == 32 and signal.width <= 32)
                    or (width_group == 64 and 32 < signal.width <= 64)
                    or (width_group == 0 and signal.width > 64)
                )
            ]
            lines.append(f"        if constexpr ({condition}) {{")
            lines.append("            switch (id) {")
            for signal in group:
                index = indices[signal.hdl_path]
                name = hierarchy_export_name(manifest, index, operation)
                lines.append(f"                case {index}:")
                value = (
                    "value" if width_group else "value.words().data()"
                )
                cached = (
                    operation == "force"
                    and signal.hdl_path in immediate_force_get_paths
                )
                invalidate_logic = (
                    operation == "force"
                    and signal.hdl_path in immediate_logic_force_get_paths
                )
                if cached or invalidate_logic:
                    lines.append("                {")
                lines.append(f"                    {name}(index, {value});")
                if cached:
                    lines.extend(
                        [
                            "                    using Cache =",
                            f"                        HierarchyImmediateForceCache<{index},",
                            "                            cpptb::probe::Value<Width>>;",
                            "                    Cache::value = value;",
                            "                    Cache::index = index;",
                            "                    Cache::callback_epoch =",
                            "                        cpptb::probe::detail::current_callback_epoch();",
                        ]
                    )
                if invalidate_logic:
                    lines.extend(
                        [
                            "                    using LogicCache =",
                            f"                        HierarchyImmediateForceCache<{index},",
                            f"                            cpptb::LogicBits<{signal.width}>>;",
                            "                    LogicCache::callback_epoch =",
                            "                        LogicCache::invalid_callback_epoch;",
                        ]
                    )
                lines.append("                    return;")
                if cached or invalidate_logic:
                    lines.append("                }")
            lines.extend(
                ["                default: break;", "            }", "        }"]
            )
        lines.extend(
            [
                f'        fail("{operation}", id);',
                "    }",
                "",
            ]
        )

    for operation in ("get", "deposit"):
        span_type = (
            "std::span<cpptb::probe::Value<Width>>"
            if operation == "get"
            else "std::span<const cpptb::probe::Value<Width>>"
        )
        lines.extend(
            [
                "    template <std::size_t Width>",
                f"    static void {operation}_span(std::uint32_t id,",
                "                            std::int32_t first_index,",
                f"                            {span_type} values) {{",
                "        constexpr std::size_t kBlockEntries = 4;",
                "        if constexpr (Width <= 64) {",
                "            switch (id) {",
            ]
        )
        for signal in selected[operation]:
            if len(signal.unpacked) != 1 or signal.width > 64:
                continue
            index = indices[signal.hdl_path]
            name = hierarchy_export_name(
                manifest, index, f"{operation}_block4"
            )
            word_count = 4 if signal.width <= 32 else 8
            lines.extend(
                [
                    f"                case {index}: {{",
                    f"                    static_assert(Width == {signal.width});",
                    "                    for (std::size_t offset = 0;",
                    "                         offset < values.size();",
                    "                         offset += kBlockEntries) {",
                    "                        const std::size_t count = std::min(",
                    "                            kBlockEntries, values.size() - offset);",
                    f"                        std::array<std::uint32_t, {word_count}> words{{}};",
                ]
            )
            if operation == "deposit":
                lines.extend(
                    [
                        "                        for (std::size_t word = 0;",
                        "                             word < count; ++word) {",
                    ]
                )
                if signal.width <= 32:
                    lines.append(
                        "                            words[word] = "
                        "static_cast<std::uint32_t>(values[offset + word]);"
                    )
                else:
                    lines.extend(
                        [
                            "                            const std::uint64_t value =",
                            "                                static_cast<std::uint64_t>(",
                            "                                    values[offset + word]);",
                            "                            words[word * 2] =",
                            "                                static_cast<std::uint32_t>(value);",
                            "                            words[word * 2 + 1] =",
                            "                                static_cast<std::uint32_t>(value >> 32);",
                        ]
                    )
                lines.extend(
                    [
                        "                        }",
                        f"                        {name}(",
                        "                            static_cast<int>(first_index + offset),",
                        "                            static_cast<int>(count), words.data());",
                    ]
                )
            else:
                lines.extend(
                    [
                        f"                        {name}(",
                        "                            static_cast<int>(first_index + offset),",
                        "                            static_cast<int>(count), words.data());",
                        "                        for (std::size_t word = 0;",
                        "                             word < count; ++word) {",
                    ]
                )
                if signal.width <= 32:
                    lines.append(
                        "                            values[offset + word] = "
                        "static_cast<cpptb::probe::Value<Width>>(words[word]);"
                    )
                else:
                    lines.extend(
                        [
                            "                            values[offset + word] =",
                            "                                static_cast<cpptb::probe::Value<Width>>(",
                            "                                    static_cast<std::uint64_t>(",
                            "                                        words[word * 2]) |",
                            "                                    (static_cast<std::uint64_t>(",
                            "                                         words[word * 2 + 1])",
                            "                                     << 32));",
                        ]
                    )
                lines.append("                        }")
            lines.extend(
                [
                    "                    }",
                    "                    return;",
                    "                }",
                ]
            )
        lines.extend(
            [
                "                default: break;",
                "            }",
                "        }",
                f'        fail("{operation}_span", id);',
                "    }",
                "",
            ]
        )

    lines.extend(
        [
            "    template <std::size_t Width>",
            "    static cpptb::LogicBits<Width> get_logic(",
            "        std::uint32_t id, std::int32_t index) {",
            "        switch (id) {",
        ]
    )
    for signal in selected["get_logic"]:
        index = indices[signal.hdl_path]
        name = hierarchy_export_name(manifest, index, "get_logic")
        cached = signal.hdl_path in immediate_logic_force_get_paths
        lines.extend(
            [
                f"            case {index}: {{",
            ]
        )
        if cached:
            lines.extend(
                [
                    "                using Cache =",
                    f"                    HierarchyImmediateForceCache<{index},",
                    "                        cpptb::LogicBits<Width>>;",
                    "                if (Cache::callback_epoch ==",
                    "                        cpptb::probe::detail::current_callback_epoch() &&",
                    "                    Cache::index == index) {",
                    "                    return Cache::value;",
                    "                }",
                ]
            )
        lines.extend(
            [
                "                std::array<svLogicVecVal,",
                "                           cpptb::LogicBits<Width>::word_count>",
                "                    words{};",
                f"                {name}(index, words.data());",
                "                return cpptb::LogicBits<Width>::from_dpi_words(",
                "                    words.data());",
                "            }",
            ]
        )
    lines.extend(
        [
            "            default: break;",
            "        }",
            '        fail("get_logic", id);',
            "    }",
            "",
        ]
    )

    for operation in ("deposit_logic", "force_logic"):
        lines.extend(
            [
                "    template <std::size_t Width>",
                f"    static void {operation}(",
                "        std::uint32_t id, std::int32_t index,",
                "        cpptb::LogicBits<Width> value) {",
                "        switch (id) {",
            ]
        )
        for signal in selected[operation]:
            index = indices[signal.hdl_path]
            name = hierarchy_export_name(manifest, index, operation)
            cached = (
                operation == "force_logic"
                and signal.hdl_path in immediate_logic_force_get_paths
            )
            invalidate_bits = (
                operation == "force_logic"
                and signal.hdl_path in immediate_force_get_paths
            )
            lines.extend(
                [
                    f"            case {index}: {{",
                    "                auto words =",
                    "                    value.template dpi_words<svLogicVecVal>();",
                    f"                {name}(index, words.data());",
                ]
            )
            if cached:
                lines.extend(
                    [
                        "                using Cache =",
                        f"                    HierarchyImmediateForceCache<{index},",
                        "                        cpptb::LogicBits<Width>>;",
                        "                Cache::value = value;",
                        "                Cache::index = index;",
                        "                Cache::callback_epoch =",
                        "                    cpptb::probe::detail::current_callback_epoch();",
                    ]
                )
            if invalidate_bits:
                lines.extend(
                    [
                        "                using BitCache =",
                        f"                    HierarchyImmediateForceCache<{index},",
                        f"                        cpptb::probe::Value<{signal.width}>>;",
                        "                BitCache::callback_epoch =",
                        "                    BitCache::invalid_callback_epoch;",
                    ]
                )
            lines.extend(["                return;", "            }"])
        lines.extend(
            [
                "            default: break;",
                "        }",
                f'        fail("{operation}", id);',
                "    }",
                "",
            ]
        )

    lines.extend(
        [
            "    static void release(std::uint32_t id, std::int32_t index) {",
            "        switch (id) {",
        ]
    )
    for signal in selected["release"]:
        index = indices[signal.hdl_path]
        name = hierarchy_export_name(manifest, index, "release")
        invalidate_bits = signal.hdl_path in immediate_force_get_paths
        invalidate_logic = (
            signal.hdl_path in immediate_logic_force_get_paths
        )
        lines.extend(
            [
                f"            case {index}: {{",
                f"                {name}(index);",
            ]
        )
        if invalidate_bits:
            lines.extend(
                [
                    "                using Cache =",
                    f"                    HierarchyImmediateForceCache<{index},",
                    f"                        cpptb::probe::Value<{signal.width}>>;",
                    "                if (Cache::index == index) {",
                    "                    Cache::callback_epoch =",
                    "                        Cache::invalid_callback_epoch;",
                    "                }",
                ]
            )
        if invalidate_logic:
            lines.extend(
                [
                    "                using LogicCache =",
                    f"                    HierarchyImmediateForceCache<{index},",
                    f"                        cpptb::LogicBits<{signal.width}>>;",
                    "                if (LogicCache::index == index) {",
                    "                    LogicCache::callback_epoch =",
                    "                        LogicCache::invalid_callback_epoch;",
                    "                }",
                ]
            )
        lines.extend(["                return;", "            }"])
    lines.extend(
        [
            "            default: break;",
            "        }",
            '        fail("release", id);',
            "    }",
            "",
            "    static cpptb::coro::Signal signal(std::uint32_t id,",
            "                                      const char* name) {",
            "        switch (id) {",
        ]
    )
    for path in hierarchy_edge_paths(accesses):
        lines.extend(
            [
                f"            case {indices[path]}:",
                "                return {nullptr, "
                f"{hierarchy_edge_signal_id(path)}, name}};",
            ]
        )
    lines.extend(
        [
            "            default: break;",
            "        }",
            '        fail("edge", id);',
            "    }",
            "",
            "private:",
            "    [[noreturn]] static void fail(const char* operation,",
            "                                  std::uint32_t id) {",
            "        std::fprintf(stderr,",
            '                     "cpptb: hierarchy %s was not selected for signal %u\\n",',
            "                     operation, id);",
            "        std::abort();",
            "    }",
            "};",
            "",
        ]
    )
    return lines


def render_cpp_hierarchy(
    hierarchy: HierarchyCatalog,
    accesses: list[dict[str, str]],
    manifest: dict[str, Any],
    packed_types: PackedCppRegistry,
) -> tuple[list[str], list[str]]:
    if not hierarchy.signals and not hierarchy.scopes:
        return [], []
    node_paths, signals_by_parent, parameters_by_parent = hierarchy_nodes(
        hierarchy
    )
    signal_indices = {
        signal.hdl_path: index
        for index, signal in enumerate(hierarchy.signals)
    }
    definitions = render_cpp_hierarchy_transport(
        hierarchy, accesses, manifest
    )
    definitions.extend(
        [
            *(f"struct {hierarchy_scope_type(path)};" for path in sorted(
                (path for path in node_paths if path)
            )),
            "",
        ]
    )

    direct_children_by_parent: dict[
        tuple[str, ...], list[tuple[str, tuple[str, ...]]]
    ] = {}
    array_groups: dict[
        tuple[tuple[str, ...], str], list[tuple[int, tuple[str, ...]]]
    ] = {}
    for candidate in node_paths:
        if not candidate:
            continue
        parent = candidate[:-1]
        component = candidate[-1]
        match = re.fullmatch(r"(.+)\[(-?\d+)\]", component)
        if match:
            array_groups.setdefault((parent, match.group(1)), []).append(
                (int(match.group(2)), candidate)
            )
        else:
            direct_children_by_parent.setdefault(parent, []).append(
                (component, candidate)
            )

    def signal_signature(signal: HierarchySignal) -> tuple[Any, ...]:
        return (
            signal.symbol_kind,
            signal.width,
            signal.type_kind,
            signal.signed,
            signal.four_state,
            tuple((item.left, item.right) for item in signal.unpacked),
            (
                signal.packed_type.structural_signature()
                if signal.packed_type is not None
                else None
            ),
        )

    @dataclass(frozen=True)
    class SelectedScopeSchema:
        paths: tuple[tuple[str, ...], ...]
        children: tuple[tuple[str, Any], ...]
        signals: tuple[tuple[str, tuple[HierarchySignal, ...]], ...]
        parameters: tuple[
            tuple[str, tuple[HierarchyParameter, ...]], ...
        ]

    schema_cache: dict[
        tuple[tuple[str, ...], ...], SelectedScopeSchema | None
    ] = {}

    def selected_scope_schema(
        paths: tuple[tuple[str, ...], ...]
    ) -> SelectedScopeSchema | None:
        cached = schema_cache.get(paths)
        if paths in schema_cache:
            return cached

        child_maps = [
            {
                name: child
                for name, child in direct_children_by_parent.get(path, [])
            }
            for path in paths
        ]
        child_names = set(child_maps[0]) if child_maps else set()
        if any(set(items) != child_names for items in child_maps[1:]):
            schema_cache[paths] = None
            return None
        if any(
            any(parent == path for parent, _ in array_groups)
            for path in paths
        ):
            schema_cache[paths] = None
            return None

        signal_maps = [
            {
                signal.cpp_path[-1]: signal
                for signal in signals_by_parent.get(path, [])
            }
            for path in paths
        ]
        signal_names = set(signal_maps[0]) if signal_maps else set()
        if any(set(items) != signal_names for items in signal_maps[1:]):
            schema_cache[paths] = None
            return None
        signal_groups: list[tuple[str, tuple[HierarchySignal, ...]]] = []
        for name in sorted(signal_names):
            grouped = tuple(items[name] for items in signal_maps)
            if any(
                signal_signature(signal) != signal_signature(grouped[0])
                for signal in grouped[1:]
            ):
                schema_cache[paths] = None
                return None
            signal_groups.append((name, grouped))

        parameter_maps = [
            {
                parameter.cpp_path[-1]: parameter
                for parameter in parameters_by_parent.get(path, [])
            }
            for path in paths
        ]
        parameter_names = set(parameter_maps[0]) if parameter_maps else set()
        if any(
            set(items) != parameter_names for items in parameter_maps[1:]
        ):
            schema_cache[paths] = None
            return None
        parameter_groups = tuple(
            (name, tuple(items[name] for items in parameter_maps))
            for name in sorted(parameter_names)
        )

        child_groups: list[tuple[str, SelectedScopeSchema]] = []
        for name in sorted(child_names):
            child_paths = tuple(items[name] for items in child_maps)
            child_schema = selected_scope_schema(child_paths)
            if child_schema is None:
                schema_cache[paths] = None
                return None
            child_groups.append((name, child_schema))

        schema = SelectedScopeSchema(
            paths,
            tuple(child_groups),
            tuple(signal_groups),
            parameter_groups,
        )
        schema_cache[paths] = schema
        return schema

    compatible_arrays: dict[
        tuple[tuple[str, ...], str], SelectedScopeSchema
    ] = {}
    for key, entries in array_groups.items():
        paths = tuple(path for _, path in sorted(entries))
        schema = selected_scope_schema(paths)
        if schema is not None:
            compatible_arrays[key] = schema

    emitted_view_types: set[str] = set()

    def render_selected_view(
        type_name: str, schema: SelectedScopeSchema
    ) -> list[str]:
        if type_name in emitted_view_types:
            return []
        rendered: list[str] = []
        child_types: list[tuple[str, str, SelectedScopeSchema]] = []
        for name, child_schema in schema.children:
            child_type = f"{type_name}{cpp_identifier(name, pascal=True)}"
            rendered.extend(render_selected_view(child_type, child_schema))
            child_types.append((name, child_type, child_schema))

        emitted_view_types.add(type_name)
        rendered.append(f"struct {type_name} {{")
        for name, child_type, _ in child_types:
            rendered.append(
                f"    [[no_unique_address]] {child_type} "
                f"{cpp_identifier(name)};"
            )
        for name, grouped in schema.signals:
            rendered.append(
                "    "
                + selected_hierarchy_signal_cpp_type(grouped, packed_types)
                + f" {cpp_identifier(name)};"
            )
        for name, _ in schema.parameters:
            rendered.append(f"    std::int64_t {cpp_identifier(name)};")
        rendered.extend(["};", ""])
        return rendered

    def selected_view_initializer(
        schema: SelectedScopeSchema, selected: int, indent: int
    ) -> list[str]:
        prefix = " " * indent
        values: list[list[str]] = []
        for _, child_schema in schema.children:
            values.append(selected_view_initializer(child_schema, selected, indent + 4))
        for _, grouped in schema.signals:
            signal = grouped[selected]
            signal_index = signal_indices[signal.hdl_path]
            values.append(
                [
                    " " * (indent + 4)
                    + f'{{{signal_index}, "{signal.hdl_path}"}}'
                ]
            )
        for _, grouped in schema.parameters:
            values.append(
                [" " * (indent + 4) + str(grouped[selected].value)]
            )
        lines = [prefix + "{"]
        for index, value_lines in enumerate(values):
            if index + 1 < len(values):
                value_lines[-1] += ","
            lines.extend(value_lines)
        lines.append(prefix + "}")
        return lines

    for key, schema in sorted(compatible_arrays.items()):
        parent, base_name = key
        array_type = hierarchy_scope_array_type(parent, base_name)
        view_type = f"{array_type}Element"
        definitions.extend(render_selected_view(view_type, schema))
        entries = sorted(array_groups[key])
        element_types = ", ".join(
            "cpptb::hierarchy::ScopeElement<"
            f"{index}, {hierarchy_scope_type(path)}>"
            for index, path in entries
        )
        definitions.extend(
            [
                f"struct {array_type} {{",
                f"    using Compatibility = cpptb::hierarchy::ScopeArray<{element_types}>;",
                "",
                "    template <std::int32_t Index>",
                "    [[nodiscard]] constexpr auto at() const {",
                "        return Compatibility{}.template at<Index>();",
                "    }",
                "",
                f"    [[nodiscard]] {view_type} operator[](",
                "        std::int32_t index) const {",
                "        switch (index) {",
            ]
        )
        for selected, (index, _) in enumerate(entries):
            definitions.append(f"            case {index}: return {view_type}")
            initializer = selected_view_initializer(schema, selected, 16)
            initializer[0] = "                " + initializer[0].lstrip()
            initializer[-1] += ";"
            definitions.extend(initializer)
        path_label = ".".join((*parent, base_name))
        valid_indices = ", ".join(str(index) for index, _ in entries)
        definitions.extend(
            [
                "            default:",
                "                std::fprintf(stderr,",
                f'                    "cpptb: hierarchy scope array \'{path_label}\' "',
                '                    "index %d is out of range; valid indices "',
                f'                    "are {valid_indices}\\n", index);',
                "                std::abort();",
                "        }",
                "    }",
                "};",
                "",
            ]
        )

    def direct_children(path: tuple[str, ...]) -> list[tuple[str, ...]]:
        return sorted(
            candidate
            for candidate in node_paths
            if len(candidate) == len(path) + 1
            and candidate[:-1] == path
        )

    def member_lines(path: tuple[str, ...]) -> list[str]:
        lines: list[str] = []
        children = direct_children(path)
        array_children: dict[str, list[tuple[int, tuple[str, ...]]]] = {}
        scalar_children: list[tuple[str, tuple[str, ...]]] = []
        for child in children:
            component = child[-1]
            match = re.fullmatch(r"(.+)\[(-?\d+)\]", component)
            if match:
                array_children.setdefault(match.group(1), []).append(
                    (int(match.group(2)), child)
                )
            else:
                scalar_children.append((component, child))

        for component, child in scalar_children:
            lines.append(
                "    [[no_unique_address]] "
                f"{hierarchy_scope_type(child)} {cpp_identifier(component)};"
            )
        for base_name, entries in sorted(array_children.items()):
            key = (path, base_name)
            if key in compatible_arrays:
                field_type = hierarchy_scope_array_type(path, base_name)
            else:
                element_types = ", ".join(
                    "cpptb::hierarchy::ScopeElement<"
                    f"{index}, {hierarchy_scope_type(child)}>"
                    for index, child in sorted(entries)
                )
                field_type = f"cpptb::hierarchy::ScopeArray<{element_types}>"
            lines.append(
                f"    [[no_unique_address]] {field_type} "
                f"{cpp_identifier(base_name)};"
            )
        for parameter in sorted(
            parameters_by_parent.get(path, []), key=lambda item: item.hdl_path
        ):
            lines.append(
                f"    static constexpr std::int64_t "
                f"{cpp_identifier(parameter.cpp_path[-1])} = "
                f"{parameter.value};"
            )
        for signal in sorted(
            signals_by_parent.get(path, []), key=lambda item: item.hdl_path
        ):
            lines.append(
                "    [[no_unique_address]] "
                f"{hierarchy_signal_cpp_type(signal, signal_indices[signal.hdl_path], packed_types)} "
                f"{cpp_identifier(signal.cpp_path[-1])};"
            )
        return lines

    for path in sorted(
        (path for path in node_paths if path),
        key=lambda item: (-len(item), item),
    ):
        definitions.append(f"struct {hierarchy_scope_type(path)} {{")
        definitions.extend(member_lines(path))
        definitions.extend(["};", ""])
    return definitions, member_lines(())


def hierarchy_signal_member_expression(signal: HierarchySignal) -> str:
    """Render a typed member expression from the elaborated hierarchy path."""

    expression = "(*this)"
    for component in signal.cpp_path[:-1]:
        match = re.fullmatch(r"(.+)\[(-?\d+)\]", component)
        if match:
            expression += (
                f".{cpp_identifier(match.group(1))}.template "
                f"at<{int(match.group(2))}>()"
            )
        else:
            expression += f".{cpp_identifier(component)}"
    expression += f".{cpp_identifier(signal.cpp_path[-1])}"
    return expression


def render_cpp_hierarchy_lookup(hierarchy: HierarchyCatalog) -> list[str]:
    if not hierarchy.signals:
        return []
    lines = [
        "    template <cpptb::hierarchy::FixedString Path>",
        "    [[nodiscard]] constexpr auto cpptb_signal() const {",
    ]
    for index, signal in enumerate(hierarchy.signals):
        keyword = "if" if index == 0 else "else if"
        lines.extend(
            [
                f"        {keyword} constexpr (Path.view() == "
                f"{json.dumps(signal.hdl_path)}) {{",
                f"            return {hierarchy_signal_member_expression(signal)};",
                "        }",
            ]
        )
    lines.extend(
        [
            "        else {",
            "            static_assert(Path.view().empty(),",
            '                          "HDL path is not present in the generated DUT hierarchy");',
            "            return cpptb::hierarchy::UnsupportedSignal{};",
            "        }",
            "    }",
        ]
    )
    return lines


def render_cpp_dut(
    ports: list[Port],
    internals: list[Internal],
    root: TreeNode,
    manifest: dict[str, Any],
    source: str,
    hierarchy: HierarchyCatalog | None = None,
    interfaces: tuple[InterfacePort, ...] = (),
) -> str:
    hierarchy = hierarchy or HierarchyCatalog()
    packed_types = collect_packed_cpp_types(ports, hierarchy)
    static_binding = static_binding_enabled(manifest)
    lines = [
        generated_banner(source).rstrip(),
        "#pragma once",
        "",
        "#include <algorithm>",
        "#include <array>",
        "#include <cstddef>",
        "#include <cstdint>",
        "#include <span>",
        "#include <string_view>",
        "",
        (
            '#include "cpptb/dpi_static_binding.hpp"'
            if static_binding
            else '#include "cpptb/coro_runtime.hpp"'
        ),
    ]
    if internals:
        lines.append('#include "cpptb/probe.hpp"')
    if hierarchy.signals or hierarchy.scopes:
        lines.append('#include "cpptb/hierarchy.hpp"')
    if any(port.direction == "inout" for port in ports):
        lines.append('#include "cpptb/inout.hpp"')
    if any(
        access["operation"].endswith("_logic")
        for access in manifest.get("hierarchy_accesses", [])
    ) or any(port.direction == "inout" and port.width > 64 for port in ports):
        lines.append('#include "svdpi.h"')
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
    writable_names = writable_port_names(ports, manifest)
    packed_transport_offsets = static_packed_transport_offsets(ports, manifest)
    port_indices = {port.name: index for index, port in enumerate(ports)}
    hierarchy_edges = hierarchy_edge_paths(
        list(manifest.get("hierarchy_accesses", []))
    )
    if all(port_word_count(port) == 1 for port in ports):
        lines.extend(f"    {signal_id(port.name)}," for port in ports)
        lines.extend(
            f"    {hierarchy_edge_signal_id(path)},"
            for path in hierarchy_edges
        )
        lines.extend(["    kCpptbSignalCount,", "};", ""])
    else:
        lines.extend(
            f"    {signal_id(port.name)} = {offset},"
            for port, offset in zip(ports, offsets)
        )
        lines.extend(
            f"    {hierarchy_edge_signal_id(path)} = {offsets[-1] + index},"
            for index, path in enumerate(hierarchy_edges)
        )
        lines.extend(
            [
                f"    kCpptbSignalCount = "
                f"{offsets[-1] + len(hierarchy_edges)},",
                "};",
                "",
            ]
        )

    lines.extend(render_packed_cpp_types(packed_types))
    lines.extend(cpp_inout_helpers(ports, manifest))
    hierarchy_definitions, hierarchy_root_members = render_cpp_hierarchy(
        hierarchy,
        list(manifest.get("hierarchy_accesses", [])),
        manifest,
        packed_types,
    )
    lines.extend(hierarchy_definitions)

    interfaces_by_path = {(interface.name,): interface for interface in interfaces}

    def field_type_for(child: Port | Internal | TreeNode) -> str:
        if isinstance(child, Port):
            observed_type = (
                cpp_static_signal_type(
                    child,
                    writable_names,
                    driven_names,
                    packed_transport_offsets,
                )
                if static_binding
                else cpp_signal_type(child, driven_names)
            )
            if child.direction == "inout":
                return cpp_inout_type(
                    child, observed_type, port_indices[child.name]
                )
            return observed_type
        if isinstance(child, Internal):
            return cpp_internal_type(child)
        return node_type(child, manifest)

    def selected_type(value_type: str, depth: int) -> str:
        result = value_type
        for _ in range(depth):
            result = (
                "decltype(std::declval<" + result + ">()[std::int32_t{}])"
            )
        return result

    def render_interface_array_node(
        node: TreeNode, interface: InterfacePort
    ) -> list[str]:
        children = list(node.children.items())
        if not children or not all(
            isinstance(child, Port) for _, child in children
        ):
            raise CodegenError(
                f"interface array {interface.name!r} has an invalid generated "
                "C++ member tree"
            )
        rank = len(interface.unpacked)
        if rank == 0:
            raise CodegenError(
                f"scalar interface {interface.name!r} reached array rendering"
            )
        types = {
            name: field_type_for(child)
            for name, child in children
            if isinstance(child, Port)
        }
        rendered = [f"struct {node_type(node, manifest)} {{"]
        for name, _ in children:
            rendered.append(
                f"    {types[name]} cpptb_{cpp_identifier(name)};"
            )

        for depth in range(rank, 0, -1):
            selection_name = f"Selection{depth}"
            rendered.append(f"    struct {selection_name} {{")
            final = depth == rank
            for name, _ in children:
                member_name = (
                    cpp_identifier(name)
                    if final
                    else f"cpptb_{cpp_identifier(name)}"
                )
                rendered.append(
                    f"        {selected_type(types[name], depth)} {member_name};"
                )
            if not final:
                rendered.extend(
                    [
                        "",
                        f"        [[nodiscard]] Selection{depth + 1} "
                        "operator[](std::int32_t index) const {",
                        f"            return Selection{depth + 1}{{",
                    ]
                )
                for index, (name, _) in enumerate(children):
                    comma = "," if index + 1 < len(children) else ""
                    rendered.append(
                        "                cpptb_"
                        f"{cpp_identifier(name)}[index]{comma}"
                    )
                rendered.extend(["            };", "        }"])
            rendered.append("    };")

        rendered.extend(
            [
                "",
                "    [[nodiscard]] Selection1 "
                "operator[](std::int32_t index) const {",
                "        return Selection1{",
            ]
        )
        for index, (name, _) in enumerate(children):
            comma = "," if index + 1 < len(children) else ""
            rendered.append(
                f"            cpptb_{cpp_identifier(name)}[index]{comma}"
            )
        rendered.extend(
            [
                "        };",
                "    }",
                "",
                "    [[nodiscard]] Selection1 at(std::int32_t index) const {",
                "        return (*this)[index];",
                "    }",
                "};",
                "",
            ]
        )
        return rendered

    for node in collect_structs(root, manifest, writable_names):
        interface = interfaces_by_path.get(node.path)
        if interface is not None and interface.unpacked:
            lines.extend(render_interface_array_node(node, interface))
            continue
        lines.append(f"struct {node_type(node, manifest)} {{")
        if static_binding and not node.path:
            lines.append("    static constexpr bool cpptb_static_binding = true;")
        for name, child in node.children.items():
            field_type = field_type_for(child)
            lines.append(f"    {field_type} {name};")
        if not node.path:
            lines.extend(hierarchy_root_members)
            lines.extend(render_cpp_hierarchy_lookup(hierarchy))
        lines.extend(["};", ""])

    compatibility_root = manifest.get("compatibility_root_type")
    if compatibility_root and compatibility_root != manifest["root_type"]:
        validate_identifier(compatibility_root, "compatibility root type")
        generated_types = {
            node_type(node, manifest)
            for node in collect_structs(root, manifest, writable_names)
        }
        if compatibility_root in generated_types:
            raise CodegenError(
                f"compatibility root type {compatibility_root!r} collides "
                "with a generated hierarchy type"
            )
        lines.extend(
            [
                f"using {compatibility_root} = {manifest['root_type']};",
                "",
            ]
        )

    lines.append(f"}}  // namespace {manifest['namespace']}")
    lines.append("")
    return "\n".join(lines)


def render_binding_expr(
    node: TreeNode,
    writable_names: set[str],
    driven_names: set[str],
    internal_indices: dict[Internal, int],
    on_demand_indices: dict[Port, int],
    port_indices: dict[Port, int],
    static_packed_offsets: dict[str, int] | None = None,
    indent: int = 4,
    bind_internals: bool = True,
) -> list[str]:
    prefix = " " * indent
    lines = [prefix + "{"]
    values = list(node.children.values())
    for index, child in enumerate(values):
        comma = "," if index + 1 < len(values) else ""
        if isinstance(child, Port):
            def append_port(expression: str) -> None:
                if child.direction == "inout":
                    expression = (
                        f"cpptb_make_inout_{port_indices[child]}({expression})"
                    )
                lines.append(" " * (indent + 4) + expression + comma)

            writable = "true" if child.name in writable_names else "false"
            driven = "true" if child.name in driven_names else "false"
            if static_packed_offsets is not None:
                dimensions = ", ".join(
                    f"coro::ArrayDimension<{dimension.left}, "
                    f"{dimension.right}>"
                    for dimension in child.unpacked
                )
                if child.transport == "packed":
                    spec_name = (
                        "StaticPackedArraySpec"
                        if child.unpacked
                        else "StaticPackedSignalSpec"
                    )
                    template_values = (
                        f"{child.width}, {writable}, {driven}, "
                        f"{signal_id(child.name)}, "
                        f"{static_packed_offsets[child.name]}"
                    )
                else:
                    spec_name = (
                        "StaticOnDemandArraySpec"
                        if child.unpacked
                        else "StaticOnDemandSignalSpec"
                    )
                    on_demand_index = on_demand_indices[child]
                    setter = (
                        f"on_demand_port_{on_demand_index}_set_words"
                        if writable == "true"
                        else "nullptr"
                    )
                    template_values = (
                        f"{child.width}, {writable}, {driven}, "
                        f"{signal_id(child.name)}, "
                        f"on_demand_port_{on_demand_index}_get_words, {setter}"
                    )
                if dimensions:
                    template_values += f", {dimensions}"
                expression = (
                    f"make_signal(cpptb::dpi::{spec_name}<{template_values}>{{}}, "
                    f'"{child.name}")'
                )
                append_port(expression)
            elif child.unpacked:
                declared_range = child.unpacked[0]
                if len(child.unpacked) == 1:
                    transport_spec = (
                        f"coro::ArraySpec<{child.width}, "
                        f"{declared_range.left}, {declared_range.right}, "
                        f"{writable}>{{}}"
                    )
                    signal_spec = render_signal_spec(
                        child, transport_spec, on_demand_indices, writable
                    )
                    expression = (
                        f"make_signal({signal_spec}, {signal_id(child.name)}, "
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
                    transport_spec = (
                        f"coro::ArraySpec<{transport_width}, "
                        f"{declared_range.left}, {declared_range.right}, "
                        f"{writable}>{{}}"
                    )
                    signal_spec = render_signal_spec(
                        child, transport_spec, on_demand_indices, writable
                    )
                    expression = (
                        f"coro::reshape_fixed_array(coro::FixedArraySpec<"
                        f"{child.width}, {writable}, {dimensions}>{{}}, "
                        f"make_signal({signal_spec}, {signal_id(child.name)}, "
                        f'"{child.name}"))'
                    )
                append_port(expression)
            elif child.width <= 32:
                if child.transport == "packed":
                    expression = (
                        f'make_signal({signal_id(child.name)}, "{child.name}")'
                    )
                else:
                    transport_spec = (
                        f"coro::SignalSpec<{child.width}, {writable}>{{}}"
                    )
                    signal_spec = render_signal_spec(
                        child, transport_spec, on_demand_indices, writable
                    )
                    expression = (
                        f"make_signal({signal_spec}, {signal_id(child.name)}, "
                        f'"{child.name}")'
                    )
                append_port(expression)
            else:
                transport_spec = (
                    f"coro::SignalSpec<{child.width}, {writable}>{{}}"
                )
                signal_spec = render_signal_spec(
                    child, transport_spec, on_demand_indices, writable
                )
                expression = (
                    f"make_signal({signal_spec}, {signal_id(child.name)}, "
                    f'"{child.name}")'
                )
                append_port(expression)
        elif isinstance(child, Internal):
            expression = (
                f"make_internal_{internal_indices[child]}()"
                if bind_internals
                else "{}"
            )
            lines.append(" " * (indent + 4) + expression + comma)
        else:
            child_lines = render_binding_expr(
                child,
                writable_names,
                driven_names,
                internal_indices,
                on_demand_indices,
                port_indices,
                static_packed_offsets,
                indent + 4,
                bind_internals,
            )
            child_lines[-1] += comma
            lines.extend(child_lines)
    lines.append(prefix + "}")
    return lines


def render_signal_spec(
    port: Port,
    transport_spec: str,
    on_demand_indices: dict[Port, int],
    writable: str,
) -> str:
    if port.transport == "packed":
        return transport_spec
    index = on_demand_indices[port]
    setter = f"on_demand_port_{index}_set_words" if writable == "true" else "nullptr"
    return (
        f"OnDemandSpec{{{transport_spec}, {port_word_count(port)}, "
        f"on_demand_port_{index}_get_words, {setter}}}"
    )


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


def cpp_port_export_declarations(
    ports: list[Port], manifest: dict[str, Any]
) -> list[str]:
    lines: list[str] = []
    for index, port in enumerate(ports):
        if port.transport != "on_demand":
            continue
        index_args = ", ".join(
            f"int index_{rank}" for rank in range(len(port.unpacked))
        )
        get_name = port_export_name(manifest, index, "get")
        if port.width <= 32:
            lines.append(f"unsigned int {get_name}({index_args});")
        elif port.width <= 64:
            lines.append(f"unsigned long long {get_name}({index_args});")
        else:
            separator = ", " if index_args else ""
            lines.append(
                f"void {get_name}({index_args}{separator}svBitVecVal* value);"
            )
        if port.direction != "input":
            continue
        set_name = port_export_name(manifest, index, "set")
        value_type = (
            "unsigned int"
            if port.width <= 32
            else (
                "unsigned long long"
                if port.width <= 64
                else "const svBitVecVal*"
            )
        )
        separator = ", " if index_args else ""
        lines.append(
            f"void {set_name}({index_args}{separator}{value_type} value);"
        )
    return lines


def cpp_on_demand_index_lines(port: Port) -> tuple[list[str], str]:
    if not port.unpacked:
        return [], ""
    lines = ["    uint32_t remaining = element;"]
    for rank in reversed(range(len(port.unpacked))):
        dimension = port.unpacked[rank]
        lines.append(
            f"    const int index_{rank} = static_cast<int>(remaining % "
            f"{dimension.size}) + {dimension.low};"
        )
        if rank != 0:
            lines.append(f"    remaining /= {dimension.size};")
    arguments = ", ".join(
        f"index_{rank}" for rank in range(len(port.unpacked))
    )
    return lines, arguments


def cpp_on_demand_helpers(
    ports: list[Port], manifest: dict[str, Any]
) -> list[str]:
    selected = on_demand_ports(ports)
    if not selected:
        return []
    lines = [
        "using OnDemandGetWordsFn = void (*)(uint32_t, uint32_t*, uint32_t);",
        "using OnDemandSetWordsFn = void (*)(uint32_t, const uint32_t*, uint32_t);",
        "",
        "template <typename TransportSpec>",
        "struct OnDemandSpec {",
        "    static constexpr bool on_demand = true;",
        "    TransportSpec transport_spec;",
        "    uint32_t word_count;",
        "    OnDemandGetWordsFn get_words_fn;",
        "    OnDemandSetWordsFn set_words_fn;",
        "};",
        "",
    ]
    for index, port in enumerate(ports):
        if port.transport != "on_demand":
            continue
        element_words = element_word_count(port)
        element_count = port_word_count(port) // element_words
        get_name = port_export_name(manifest, index, "get")
        index_lines, index_args = cpp_on_demand_index_lines(port)
        prefix = f"{index_args}, " if index_args else ""
        lines.extend(
            [
                f"inline void on_demand_port_{index}_get_words(",
                "    uint32_t word_offset, uint32_t* words, uint32_t word_count) {",
                f'    probe::detail::require_signal_callback("{port.name}");',
                f"    constexpr uint32_t kElementWords = {element_words};",
                f"    constexpr uint32_t kElementCount = {element_count};",
                "    const uint32_t element = word_offset / kElementWords;",
                "    if (!words || (word_offset % kElementWords) != 0 ||",
                "        word_count != kElementWords || element >= kElementCount) {",
                f'        std::fprintf(stderr, "cpptb: invalid on-demand get for {port.name}\\n");',
                "        std::abort();",
                "    }",
            ]
        )
        lines.extend(index_lines)
        if port.width <= 32:
            lines.append(f"    words[0] = {get_name}({index_args});")
        elif port.width <= 64:
            lines.extend(
                [
                    f"    const uint64_t value = {get_name}({index_args});",
                    "    words[0] = static_cast<uint32_t>(value);",
                    "    words[1] = static_cast<uint32_t>(value >> 32);",
                ]
            )
        else:
            lines.extend(
                [
                    f"    {get_name}({prefix}reinterpret_cast<svBitVecVal*>(words));",
                ]
            )
        lines.extend(["}", ""])

        if port.direction != "input":
            continue
        set_name = port_export_name(manifest, index, "set")
        lines.extend(
            [
                f"inline void on_demand_port_{index}_set_words(",
                "    uint32_t word_offset, const uint32_t* words, uint32_t word_count) {",
                f'    probe::detail::require_signal_callback("{port.name}");',
                f"    constexpr uint32_t kElementWords = {element_words};",
                f"    constexpr uint32_t kElementCount = {element_count};",
                "    const uint32_t element = word_offset / kElementWords;",
                "    if (!words || (word_offset % kElementWords) != 0 ||",
                "        word_count != kElementWords || element >= kElementCount) {",
                f'        std::fprintf(stderr, "cpptb: invalid on-demand set for {port.name}\\n");',
                "        std::abort();",
                "    }",
            ]
        )
        lines.extend(index_lines)
        if port.width <= 32:
            lines.append(f"    {set_name}({prefix}words[0]);")
        elif port.width <= 64:
            lines.extend(
                [
                    "    const uint64_t value = static_cast<uint64_t>(words[0]) |",
                    "        (static_cast<uint64_t>(words[1]) << 32);",
                    f"    {set_name}({prefix}value);",
                ]
            )
        else:
            lines.append(
                f"    {set_name}({prefix}reinterpret_cast<const svBitVecVal*>(words));"
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
    clocks = clock_configs(manifest)
    edge_observers = edge_observer_ports(ports, manifest)
    hierarchy_edges = hierarchy_edge_paths(
        list(manifest.get("hierarchy_accesses", []))
    )
    driven_names = driven_port_names(ports, manifest)
    observed_names = observed_port_names(ports, manifest)
    writable_names = writable_port_names(ports, manifest)
    compact_input_transport = bool(
        manifest.get("run", {}).get("compact_input_transport", True)
    )
    static_binding = static_binding_enabled(manifest)
    selected_on_demand = on_demand_ports(ports)
    registered_clocks = registered_clock_configs(manifest)
    effective_compact_input_transport = (
        compact_input_transport or bool(selected_on_demand) or static_binding
    )
    driven = [port for port in ports if port.name in driven_names]
    observed = [port for port in ports if port.name in observed_names]
    packed_driven = packed_ports(driven)
    packed_observed = packed_ports(observed)
    packed_transport_offsets = static_packed_transport_offsets(ports, manifest)
    offsets = signal_word_offsets(ports)
    include = manifest["outputs"]["cpp_include"]
    lines = [
        generated_banner(source).rstrip(),
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstdint>",
        "#include <utility>",
        '#include "cpptb/dpi_static_binding.hpp"',
    ]
    if internals or selected_on_demand:
        lines.append('#include "svdpi.h"')
    if selected_on_demand:
        lines.extend(
            [
                '#include "cpptb/probe.hpp"',
                "#include <cstdio>",
                "#include <cstdlib>",
            ]
        )
    lines.extend(["", f'#include "{include}"', ""])
    if internals or selected_on_demand:
        lines.extend(['extern "C" {'])
        declarations = [
            *cpp_port_export_declarations(ports, manifest),
            *cpp_internal_export_declarations(internals, manifest),
        ]
        lines.extend(f"    {declaration}" for declaration in declarations)
        lines.extend(["}", ""])
    lines.extend(
        [
            f"namespace {manifest['namespace']}::generated {{",
            "",
            f"inline constexpr bool kCompactInputTransport = "
            f"{'true' if effective_compact_input_transport else 'false'};",
            f"inline constexpr std::array<uint32_t, {len(clocks)}> kClockSignalIds = {{",
        ]
    )
    lines.extend(
        f"    {clock_signal_id_expression(clock)},"
        for clock in clocks
    )
    lines.extend([
        "};",
        f"inline constexpr std::array<cpptb::dpi::RegisteredClockConfig, "
        f"{len(registered_clocks)}> kRegisteredClockConfigs = {{{{",
    ])
    lines.extend(
        f"    {{{clock_signal_id_expression(clock)}, "
        f"{clock_period_femtoseconds(clock)}ULL, "
        f"{clock_phase_femtoseconds(clock)}ULL, "
        f"{int(clock.get('initial_value', 0))}u}},"
        for clock in registered_clocks
    )
    lines.extend([
        "}};",
        f"inline constexpr std::array<uint32_t, "
        f"{len(edge_observers) + len(hierarchy_edges)}> "
        "kEdgeObserverSignalIds = {",
    ])
    lines.extend(f"    {signal_id(port.name)}," for port in edge_observers)
    lines.extend(
        f"    {hierarchy_edge_signal_id(path)}," for path in hierarchy_edges
    )
    lines.extend([
        "};",
        f"inline constexpr std::array<uint32_t, {len(hierarchy_edges)}> "
        "kTransportlessEdgeSignalIds = {",
    ])
    lines.extend(
        f"    {hierarchy_edge_signal_id(path)}," for path in hierarchy_edges
    )
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
        f"inline constexpr std::array<uint32_t, {sum(port_word_count(port) for port in packed_observed)}> "
        "kObservedSignalWordIds = {",
    ])
    lines.extend(
        f"    {word_id}," for word_id in signal_word_ids(packed_observed)
    )
    lines.extend([
        "};",
        f"inline constexpr std::array<uint32_t, {sum(port_word_count(port) for port in packed_driven)}> "
        "kDrivenSignalWordIds = {",
    ])
    lines.extend(f"    {word_id}," for word_id in signal_word_ids(packed_driven))
    lines.extend(["};", ""])
    if static_binding:
        static_packed_ports = [
            *((port, False) for port in packed_observed),
            *((port, True) for port in packed_driven),
        ]
        observed_offsets = directional_transport_offsets(packed_observed)
        driven_offsets = directional_transport_offsets(packed_driven)
        lines.extend(
            [
                f"inline constexpr std::array<cpptb::dpi::StaticPackedBindingSpan, "
                f"{len(static_packed_ports)}> kStaticPackedBindingSpans = {{{{",
            ]
        )
        for port, driven_role in static_packed_ports:
            transport_offset = (
                driven_offsets[port.name]
                if driven_role
                else observed_offsets[port.name]
            )
            lines.append(
                f"    {{{signal_id(port.name)}, {port_word_count(port)}, "
                f"{transport_offset}, "
                f"{'true' if driven_role else 'false'}}},"
            )
        lines.extend(
            [
                "}};",
                "static_assert(cpptb::dpi::validate_static_packed_binding_spans(",
                "    kStaticPackedBindingSpans, kObservedSignalWordIds,",
                "    kDrivenSignalWordIds));",
                "",
            ]
        )
    lines.extend(cpp_on_demand_helpers(ports, manifest))
    if internals:
        lines.extend(cpp_internal_helpers(internals, manifest))
    lines.append("template <typename MakeSignal>")
    lines.append(
        f"{manifest['root_type']} bind_dut(MakeSignal&& make_signal) {{"
    )
    expression = render_binding_expr(
        root,
        writable_names,
        driven_names,
        {internal: index for index, internal in enumerate(internals)},
        {
            port: index
            for index, port in enumerate(ports)
            if port.transport == "on_demand"
        },
        {port: index for index, port in enumerate(ports)},
        packed_transport_offsets if static_binding else None,
        4,
    )
    expression[0] = "    return " + expression[0].lstrip()
    expression[-1] += ";"
    lines.extend(expression)
    if static_binding:
        lines.extend(["}", "", "template <typename MakeSignal>"])
        lines.append(
            f"{manifest['root_type']} bind_dut_for_clock_discovery("
            "MakeSignal&& make_signal) {"
        )
        discovery_expression = render_binding_expr(
            root,
            writable_names,
            driven_names,
            {internal: index for index, internal in enumerate(internals)},
            {
                port: index
                for index, port in enumerate(ports)
                if port.transport == "on_demand"
            },
            {port: index for index, port in enumerate(ports)},
            packed_transport_offsets,
            4,
            False,
        )
        discovery_expression[0] = (
            "    return " + discovery_expression[0].lstrip()
        )
        discovery_expression[-1] += ";"
        lines.extend(discovery_expression)
    lines.extend(["}", "", f"}}  // namespace {manifest['namespace']}::generated", ""])
    return "\n".join(lines)


def render_cpp_adapter(manifest: dict[str, Any], source: str) -> str:
    run = manifest.get("run", {})
    dynamic_clocks = bool(run.get("dynamic_clocks", False))
    outputs = manifest["outputs"]
    target = manifest.get("target", manifest["module"])
    result_token = re.sub(r"[^A-Za-z0-9]+", "_", target).upper()
    timeout_cycles = int(run.get("timeout_cycles", 1_000_000))
    if timeout_cycles <= 0:
        raise CodegenError("run.timeout_cycles must be greater than zero")

    functions = {
        "init": run.get("init_function", "cpptb_dpi_init"),
        "step": run.get("step_function", "cpptb_dpi_step"),
        "pull": run.get("pull_outputs_function", "cpptb_dpi_pull_outputs"),
        "deadline": run.get(
            "next_deadline_function", "cpptb_dpi_next_timer_deadline"
        ),
        "edge": run.get("edge_interest_function", "cpptb_dpi_edge_interest"),
        "clock": run.get("clock_config_function", "cpptb_dpi_clock_config"),
        "phase_dispatch": run.get(
            "phase_dispatch_function", "cpptb_dpi_phase_dispatch"
        ),
    }
    for label, function in functions.items():
        validate_identifier(function, f"{label} DPI function")

    namespace = manifest["namespace"]
    adapter_type = f"{namespace}::generated::DpiAdapter"
    lines = [
        generated_banner(source).rstrip(),
        "",
        '#include "cpptb/dpi_runtime.hpp"',
        '#include "cpptb/test_api.hpp"',
        f'#include "{outputs["cpp_binding_include"]}"',
        "",
        f'extern "C" void {functions["phase_dispatch"]}(unsigned int phase);',
        "",
        f"namespace {namespace}::generated {{",
        "",
        "struct DpiAdapter {",
        f"    using Dut = ::{namespace}::{manifest['root_type']};",
        "    using Result = cpptb::TestResult;",
        "",
        "    static constexpr uint32_t signal_count = kCpptbSignalCount;",
        "    static constexpr bool compact_input_transport =",
        "        kCompactInputTransport;",
        "    inline static constexpr auto driven_signal_spans =",
        "        kDrivenSignalSpans;",
        "    inline static constexpr auto observed_signal_word_ids =",
        "        kObservedSignalWordIds;",
        "    inline static constexpr auto driven_signal_word_ids =",
        "        kDrivenSignalWordIds;",
        "    inline static constexpr auto clock_signal_ids = kClockSignalIds;",
        "    inline static constexpr auto registered_clock_configs =",
        "        kRegisteredClockConfigs;",
        "    inline static constexpr auto edge_observer_signal_ids =",
        "        kEdgeObserverSignalIds;",
        "    inline static constexpr auto transportless_edge_signal_ids =",
        "        kTransportlessEdgeSignalIds;",
        f'    static constexpr const char* result_name = "CPP_DPI_{result_token}_RESULT";',
        "",
        "    template <typename MakeSignal>",
        "    static Dut bind_dut(MakeSignal make_signal) {",
        f"        return ::{namespace}::generated::bind_dut(make_signal);",
        "    }",
        "",
        "    static void register_testbench(coro::Testbench& scheduler, Dut dut,",
        "                                   uint32_t, Result& result,",
        "                                   coro::ClockRegistrar clocks,",
        "                                   cpptb::detail::SimLogEndpoint& sim_logs) {",
        "        cpptb::run_registered_test(",
        "            scheduler, dut, result, cpptb::detail::environment_run_request(),",
        "            clocks, nullptr, &sim_logs);",
        "    }",
        "",
        "    static void dispatch_phase(uint32_t phase) {",
        f"        {functions['phase_dispatch']}(phase);",
        "    }",
        "",
        "    static bool timed_out(coro::SimTime, uint64_t sim_cycles,",
        "                          uint32_t) {",
        f"        return sim_cycles > {timeout_cycles}ULL;",
        "    }",
        "};",
        "",
        f"}}  // namespace {namespace}::generated",
        "",
        "CPPTB_DEFINE_NAMED_DPI_RUNTIME(",
        f"    {adapter_type}, {functions['init']}, {functions['step']},",
        f"    {functions['pull']}, {functions['deadline']}, {functions['edge']})",
    ]
    if dynamic_clocks:
        lines.extend(
            [
                f"CPPTB_DEFINE_NAMED_DPI_CLOCK_API({functions['clock']})",
            ]
        )
    lines.append("")
    return "\n".join(lines)


def render_cpp_clock_discovery(manifest: dict[str, Any], source: str) -> str:
    outputs = manifest["outputs"]
    namespace = manifest["namespace"]
    timeprecision_fs = time_literal_femtoseconds(
        manifest.get("run", {}).get("timeprecision", "1ps"),
        "run.timeprecision",
    )
    lines = [
        generated_banner(source).rstrip(),
        "",
        '#include "cpptb/clock_discovery.hpp"',
        '#include "cpptb/hierarchy.hpp"',
        f'#include "{outputs["cpp_binding_include"]}"',
        "",
        "int main(int argc, char** argv) {",
        "    if (argc != 2 && argc != 3) {",
        '        std::fprintf(stderr, "usage: %s CLOCKS.json [ACCESS.json]\\n", argv[0]);',
        "        return 2;",
        "    }",
        f"    using Dut = ::{namespace}::{manifest['root_type']};",
        f"    const int result = cpptb::dpi::discover_registered_clocks<",
        f"        Dut, ::{namespace}::kCpptbSignalCount>(",
        f"        argv[1], {timeprecision_fs}ULL, [](auto make_signal) {{",
        f"            return ::{namespace}::generated::",
        "                bind_dut_for_clock_discovery(make_signal);",
        "        });",
        "    if (result != 0) return result;",
        "    if (argc == 3 &&",
        "        !cpptb::hierarchy::write_discovered_access_plan(argv[2])) {",
        "        return 1;",
        "    }",
        "    return 0;",
        "}",
        "",
    ]
    return "\n".join(lines)


def sv_decl(port: Port) -> str:
    if port.interface_name is not None:
        raise CodegenError(
            f"interface member {port.name!r} cannot be declared as a flat port"
        )
    packed = "" if port.width == 1 else f" [{port.width - 1}:0]"
    value_type = (
        "tri"
        if port.direction == "inout"
        else ("bit" if port.width > 32 else "logic")
    )
    unpacked = "".join(
        f" [{dimension.left}:{dimension.right}]" for dimension in port.unpacked
    )
    return f"  {value_type}{packed} {port.name}{unpacked};"


def sv_interface_bridge_name(
    interface: InterfacePort | str, member: InterfaceConstructorPort | str
) -> str:
    interface_name = (
        interface.name if isinstance(interface, InterfacePort) else interface
    )
    member_name = (
        member.name if isinstance(member, InterfaceConstructorPort) else member
    )
    return f"cpptb_{cpp_identifier(interface_name)}_{cpp_identifier(member_name)}"


def sv_inout_storage_name(port: Port, suffix: str) -> str:
    return f"cpptb_{cpp_identifier(port.name)}_{suffix}"


def sv_index_tuples(port: Port) -> list[tuple[int, ...]]:
    if not port.unpacked:
        return [()]
    return list(
        product(
            *(
                range(dimension.low, dimension.high + 1)
                for dimension in port.unpacked
            )
        )
    )


def sv_inout_declarations(port: Port) -> list[str]:
    packed = "" if port.width == 1 else f" [{port.width - 1}:0]"
    unpacked = "".join(
        f" [{dimension.left}:{dimension.right}]"
        for dimension in port.unpacked
    )
    drive_name = sv_inout_storage_name(port, "drive")
    enable_name = sv_inout_storage_name(port, "oe")
    lines = [
        f"  logic{packed} {drive_name}{unpacked};",
        f"  bit {enable_name}{unpacked};",
    ]
    for indices in sv_index_tuples(port):
        index_suffix = "".join(f"[{index}]" for index in indices)
        target = sv_port_reference(port, [str(index) for index in indices])
        lines.append(
            f"  assign {target} = {enable_name}{index_suffix} ? "
            f"{drive_name}{index_suffix} : 'z;"
        )
    return lines


def sv_interface_declarations(interface: InterfacePort) -> list[str]:
    lines: list[str] = []
    interface_ranges = "".join(
        f" [{dimension.left}:{dimension.right}]"
        for dimension in interface.unpacked
    )
    for constructor in interface.constructor_ports:
        net_type = "wire" if constructor.direction in {"output", "inout"} else (
            "logic" if constructor.four_state else "bit"
        )
        packed = "" if constructor.width == 1 else f" [{constructor.width - 1}:0]"
        unpacked = "".join(
            f" [{dimension.left}:{dimension.right}]"
            for dimension in (*interface.unpacked, *constructor.unpacked)
        )
        lines.append(
            f"  {net_type}{packed} "
            f"{sv_interface_bridge_name(interface, constructor)}{unpacked};"
        )

    parameters = ""
    if interface.parameters:
        assignments = ", ".join(
            f".{parameter.name}({parameter.value})"
            for parameter in interface.parameters
        )
        parameters = f" #({assignments})"
    lines.append(
        f"  {interface.definition}{parameters} {interface.name}"
        f"{interface_ranges} ("
    )
    for index, constructor in enumerate(interface.constructor_ports):
        comma = "," if index + 1 < len(interface.constructor_ports) else ""
        lines.append(
            f"      .{constructor.name}("
            f"{sv_interface_bridge_name(interface, constructor)}){comma}"
        )
    lines.append("  );")
    return lines


def sv_port_reference(port: Port, indices: list[str] | tuple[str, ...] = ()) -> str:
    if len(indices) != len(port.unpacked):
        raise CodegenError(
            f"port {port.name!r} reference expected {len(port.unpacked)} "
            f"indices, got {len(indices)}"
        )
    if port.interface_name is None:
        return port.name + "".join(f"[{index}]" for index in indices)

    interface_indices = indices[: port.interface_rank]
    member_indices = indices[port.interface_rank :]
    if port.interface_constructor_port:
        target = sv_interface_bridge_name(
            port.interface_name, port.interface_member or ""
        )
        return target + "".join(f"[{index}]" for index in indices)
    target = port.interface_name + "".join(
        f"[{index}]" for index in interface_indices
    )
    target += f".{port.interface_member}"
    return target + "".join(f"[{index}]" for index in member_indices)


def sv_scalar_port_reference(port: Port) -> str:
    return sv_port_reference(port)


def sv_array_contexts(
    port: Port,
) -> list[tuple[list[str], int, str, str]]:
    """Return loops and references, unrolling interface instance indices.

    Interface instance array selections are hierarchical and therefore must be
    constant in SystemVerilog. Ordinary unpacked member dimensions remain
    compact generated loops.
    """

    interface_dimensions = port.unpacked[: port.interface_rank]
    member_dimensions = port.unpacked[port.interface_rank :]
    constant_indices = (
        product(
            *(
                range(dimension.low, dimension.high + 1)
                for dimension in interface_dimensions
            )
        )
        if interface_dimensions
        else [()]
    )
    contexts: list[tuple[list[str], int, str, str]] = []
    for constants in constant_indices:
        variable_indices = [
            f"cpptb_{cpp_identifier(port.name)}_index_{rank + port.interface_rank}"
            for rank in range(len(member_dimensions))
        ]
        indices = [*(str(value) for value in constants), *variable_indices]
        lines: list[str] = []
        for rank, (dimension, index) in enumerate(
            zip(member_dimensions, variable_indices)
        ):
            lines.append(
                "    "
                + "  " * rank
                + f"for (int {index} = {dimension.low}; "
                + f"{index} <= {dimension.high}; {index}++) begin"
            )
        source = sv_port_reference(port, indices)
        linear = f"({indices[0]} - {port.unpacked[0].low})"
        for dimension, index in zip(port.unpacked[1:], indices[1:]):
            linear = (
                f"({linear} * {dimension.size} + "
                f"({index} - {dimension.low}))"
            )
        contexts.append((lines, len(variable_indices), source, linear))
    return contexts


def sv_pack_assignments(port: Port, signal: str | None = None) -> list[str]:
    signal = signal or sv_signal_constant(port)
    if port.unpacked:
        words = element_word_count(port)
        lines: list[str] = []
        for loop_lines, loop_count, source, linear in sv_array_contexts(port):
            lines.extend(loop_lines)
            body_indent = "    " + "  " * loop_count
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
                    f"{body_indent}in_words[{signal} + {offset}] = "
                    f"{word_source};"
                )
            for rank in reversed(range(loop_count)):
                lines.append("    " + "  " * rank + "end")
        return lines

    lines = []
    for word in range(element_word_count(port)):
        lsb = word * 32
        width = min(32, port.width - lsb)
        if port.width <= 32:
            source = sv_scalar_port_reference(port)
        else:
            source = f"{sv_scalar_port_reference(port)}[{lsb} +: {width}]"
        index = signal if element_word_count(port) == 1 else f"{signal} + {word}"
        lines.append(f"    in_words[{index}] = {source};")
    return lines


def sv_output_assignments(
    port: Port,
    signal: str | None = None,
    skip_linear_indices: set[int] | None = None,
) -> list[str]:
    signal = signal or sv_signal_constant(port)
    skipped = skip_linear_indices or set()
    if port.unpacked:
        words = element_word_count(port)
        lines: list[str] = []
        for loop_lines, loop_count, target, linear in sv_array_contexts(port):
            lines.extend(loop_lines)
            body_indent = "    " + "  " * loop_count
            assignment_indent = body_indent
            if skipped:
                condition = " && ".join(
                    f"(({linear}) != {index})" for index in sorted(skipped)
                )
                lines.append(f"{body_indent}if ({condition}) begin")
                assignment_indent += "  "
            offset = f"{linear} * {words}"
            if port.width > 32:
                chunks = []
                for word in reversed(range(words)):
                    lsb = word * 32
                    width = min(32, port.width - lsb)
                    word_offset = (
                        offset if word == 0 else f"{offset} + {word}"
                    )
                    source = f"out_words[{signal} + {word_offset}]"
                    if width < 32:
                        source += f"[{width - 1}:0]"
                    chunks.append(source)
                lines.append(
                    f"{assignment_indent}{target} = "
                    f"{{{', '.join(chunks)}}};"
                )
            else:
                source = f"out_words[{signal} + {offset}]"
                if port.width == 1:
                    source += "[0]"
                elif port.width < 32:
                    source += f"[{port.width - 1}:0]"
                lines.append(f"{assignment_indent}{target} = {source};")
            if skipped:
                lines.append(f"{body_indent}end")
            for rank in reversed(range(loop_count)):
                lines.append("    " + "  " * rank + "end")
        return lines

    if 0 in skipped:
        return []
    if port.width > 32:
        chunks = []
        for word in reversed(range(element_word_count(port))):
            lsb = word * 32
            width = min(32, port.width - lsb)
            source = f"out_words[{signal} + {word}]"
            if width < 32:
                source += f"[{width - 1}:0]"
            chunks.append(source)
        return [
            f"    {sv_scalar_port_reference(port)} = "
            f"{{{', '.join(chunks)}}};"
        ]

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
            target = sv_scalar_port_reference(port)
        else:
            target = f"{sv_scalar_port_reference(port)}[{lsb} +: {width}]"
        lines.append(f"    {target} = {source};")
    return lines


def sv_signal_constant(port: Port) -> str:
    name = signal_id(port.name).replace("kSignal", "SIGNAL_").upper()
    return name


def sv_clock_signal_expression(
    clock: dict[str, Any], ports: list[Port]
) -> str:
    discovered = clock.get("signal_id")
    if isinstance(discovered, int) and not isinstance(discovered, bool):
        return str(discovered)
    port, _ = resolve_clock_port(clock, ports)
    return sv_signal_constant(port)


def sv_zero_assignments(
    port: Port,
    indent: str = "    ",
    skip_linear_indices: set[int] | None = None,
) -> list[str]:
    skipped = skip_linear_indices or set()
    if not port.unpacked:
        if 0 in skipped:
            return []
        return [f"{indent}{sv_scalar_port_reference(port)} = '0;"]
    if not skipped and port.interface_name is None:
        return [f"{indent}{port.name} = '{{default: '0}};"]
    adjusted: list[str] = []
    for lines, loop_count, target, linear in sv_array_contexts(port):
        adjusted.extend(indent + line[4:] for line in lines)
        body_indent = indent + "  " * loop_count
        assignment_indent = body_indent
        if skipped:
            condition = " && ".join(
                f"(({linear}) != {index})" for index in sorted(skipped)
            )
            adjusted.append(f"{body_indent}if ({condition}) begin")
            assignment_indent += "  "
        adjusted.append(f"{assignment_indent}{target} = '0;")
        if skipped:
            adjusted.append(f"{body_indent}end")
        for rank in reversed(range(loop_count)):
            adjusted.append(indent + "  " * rank + "end")
    return adjusted


def sv_hierarchy_edge_constant(path: str) -> str:
    return hierarchy_edge_signal_id(path).replace(
        "kHierarchyEdge", "HIERARCHY_EDGE_"
    ).upper()


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


def hierarchy_memory_targets(
    signal: HierarchySignal, target: str
) -> list[tuple[int, str, str]]:
    if not signal.unpacked:
        return [(0, target, "")]
    ranges = [
        range(dimension.low, dimension.high + 1)
        for dimension in signal.unpacked
    ]
    return [
        (
            linear,
            target + "".join(f"[{index}]" for index in indices),
            "".join(f"[{index}]" for index in indices),
        )
        for linear, indices in enumerate(product(*ranges))
    ]


def hierarchy_assignment_value(
    signal: HierarchySignal, value: str
) -> str:
    packed_type = signal.packed_type
    if isinstance(packed_type, (PackedEnumType, PackedStructType)):
        if packed_type.declared_name:
            return f"{packed_type.declared_name}'({value})"
    return value


def sv_hierarchy_export_functions(
    hierarchy: HierarchyCatalog,
    accesses: list[dict[str, str]],
    manifest: dict[str, Any],
) -> list[str]:
    if not accesses:
        return []
    indices = {
        signal.hdl_path: index
        for index, signal in enumerate(hierarchy.signals)
    }
    signals = {signal.hdl_path: signal for signal in hierarchy.signals}
    operations = {
        (access["path"], access["operation"]) for access in accesses
    }
    lines: list[str] = []
    for path in sorted({access["path"] for access in accesses}):
        signal = signals[path]
        index = indices[path]
        target = f"i_dut.{path}"
        memory_targets = hierarchy_memory_targets(signal, target)
        force_selected = (
            (path, "force") in operations
            or (path, "force_logic") in operations
            or (path, "release") in operations
        )
        dynamic_one_dimensional_memory = (
            len(signal.unpacked) == 1
            and signal.unpacked[0].low == 0
            and not force_selected
        )
        value_type = (
            "int unsigned"
            if signal.width <= 32
            else (
                "longint unsigned"
                if signal.width <= 64
                else f"bit [{signal.width - 1}:0]"
            )
        )
        logic_value_type = f"logic [{signal.width - 1}:0]"
        force_shadow = f"hierarchy_{index}_force_shadow"
        if ((path, "force") in operations or
                (path, "force_logic") in operations):
            unpacked_suffix = ""
            if signal.unpacked:
                unpacked_suffix = "".join(
                    f" [{dimension.left}:{dimension.right}]"
                    for dimension in signal.unpacked
                )
            lines.extend(
                [
                    f"  {'logic' if (path, 'force_logic') in operations else 'bit'} "
                    f"[{signal.width - 1}:0] "
                    f"{force_shadow}{unpacked_suffix};",
                    "",
                ]
            )

        if (path, "get") in operations:
            name = hierarchy_export_name(manifest, index, "get")
            lines.append(f'  export "DPI-C" function {name};')
            if signal.width <= 64:
                lines.append(
                    f"  function {value_type} {name}(input int index);"
                )
            else:
                lines.append(
                    f"  function void {name}(input int index, "
                    f"output {value_type} value);"
                )
            destination = name if signal.width <= 64 else "value"
            if dynamic_one_dimensional_memory:
                lines.append(
                    f"    {destination} = $unsigned({target}[index]);"
                )
            elif len(signal.unpacked) == 1:
                lines.append("    case (index)")
                dimension = signal.unpacked[0]
                for linear, element_target, _ in memory_targets:
                    actual_index = dimension.low + linear
                    lines.append(
                        f"      {actual_index}: {destination} = "
                        f"$unsigned({element_target});"
                    )
                lines.extend(
                    [
                        f'      default: $fatal(1, "read index %0d is out of bounds", index);',
                        "    endcase",
                    ]
                )
            elif signal.unpacked:
                lines.append("    case (index)")
                for linear, element_target, _ in memory_targets:
                    lines.append(
                        f"      {linear}: {destination} = "
                        f"$unsigned({element_target});"
                    )
                lines.extend(
                    [
                        f'      default: $fatal(1, "read index %0d is out of bounds", index);',
                        "    endcase",
                    ]
                )
            else:
                lines.append(f"    {destination} = $unsigned({target});")
            lines.extend(["  endfunction", ""])
            if len(signal.unpacked) == 1 and signal.width <= 64:
                block_name = hierarchy_export_name(
                    manifest, index, "get_block4"
                )
                slot_width = 32 if signal.width <= 32 else 64
                payload_width = slot_width * 4
                lines.extend(
                    [
                        f'  export "DPI-C" function {block_name};',
                        f"  function void {block_name}(",
                        "      input int first_index, input int count,",
                        f"      output bit [{payload_width - 1}:0] values);",
                        "    values = '0;",
                        "    for (int offset = 0; offset < count; offset++) begin",
                    ]
                )
                destination_slice = (
                    f"values[offset * {slot_width} +: {signal.width}]"
                )
                if dynamic_one_dimensional_memory:
                    lines.append(
                        f"      {destination_slice} = "
                        f"$unsigned({target}[first_index + offset]);"
                    )
                else:
                    lines.append("      case (first_index + offset)")
                    dimension = signal.unpacked[0]
                    for linear, element_target, _ in memory_targets:
                        actual_index = dimension.low + linear
                        lines.append(
                            f"        {actual_index}: {destination_slice} = "
                            f"$unsigned({element_target});"
                        )
                    lines.extend(
                        [
                            '        default: $fatal(1, "block read index %0d is out of bounds", first_index + offset);',
                            "      endcase",
                        ]
                    )
                lines.extend(["    end", "  endfunction", ""])

        if (path, "get_logic") in operations:
            name = hierarchy_export_name(manifest, index, "get_logic")
            lines.extend(
                [
                    f'  export "DPI-C" function {name};',
                    f"  function void {name}(input int index, "
                    f"output {logic_value_type} value);",
                ]
            )
            if dynamic_one_dimensional_memory:
                lines.append(f"    value = {target}[index];")
            elif len(signal.unpacked) == 1:
                lines.append("    case (index)")
                dimension = signal.unpacked[0]
                for linear, element_target, _ in memory_targets:
                    actual_index = dimension.low + linear
                    lines.append(
                        f"      {actual_index}: value = {element_target};"
                    )
                lines.extend(
                    [
                        f'      default: $fatal(1, "logic read index %0d is out of bounds", index);',
                        "    endcase",
                    ]
                )
            elif signal.unpacked:
                lines.append("    case (index)")
                for linear, element_target, _ in memory_targets:
                    lines.append(
                        f"      {linear}: value = {element_target};"
                    )
                lines.extend(
                    [
                        f'      default: $fatal(1, "logic read index %0d is out of bounds", index);',
                        "    endcase",
                    ]
                )
            else:
                lines.append(f"    value = {target};")
            lines.extend(["  endfunction", ""])

        if (path, "deposit") in operations:
            name = hierarchy_export_name(manifest, index, "deposit")
            assigned_value = hierarchy_assignment_value(signal, "value")
            lines.extend(
                [
                    f'  export "DPI-C" function {name};',
                    f"  function void {name}(input int index, "
                    f"input {value_type} value);",
                ]
            )
            if dynamic_one_dimensional_memory:
                lines.append(f"    {target}[index] = {assigned_value};")
            elif len(signal.unpacked) == 1:
                lines.append("    case (index)")
                dimension = signal.unpacked[0]
                for linear, element_target, _ in memory_targets:
                    actual_index = dimension.low + linear
                    lines.append(
                        f"      {actual_index}: {element_target} = "
                        f"{assigned_value};"
                    )
                lines.extend(
                    [
                        f'      default: $fatal(1, "deposit index %0d is out of bounds", index);',
                        "    endcase",
                    ]
                )
            elif signal.unpacked:
                lines.append("    case (index)")
                for linear, element_target, _ in memory_targets:
                    lines.append(
                        f"      {linear}: {element_target} = "
                        f"{assigned_value};"
                    )
                lines.extend(
                    [
                        f'      default: $fatal(1, "deposit index %0d is out of bounds", index);',
                        "    endcase",
                    ]
                )
            else:
                lines.append(f"    {target} = {assigned_value};")
            lines.extend(["  endfunction", ""])
            if len(signal.unpacked) == 1 and signal.width <= 64:
                block_name = hierarchy_export_name(
                    manifest, index, "deposit_block4"
                )
                slot_width = 32 if signal.width <= 32 else 64
                payload_width = slot_width * 4
                block_value = (
                    f"values[offset * {slot_width} +: {signal.width}]"
                )
                assigned_block_value = hierarchy_assignment_value(
                    signal, block_value
                )
                lines.extend(
                    [
                        f'  export "DPI-C" function {block_name};',
                        f"  function void {block_name}(",
                        "      input int first_index, input int count,",
                        f"      input bit [{payload_width - 1}:0] values);",
                        "    for (int offset = 0; offset < count; offset++) begin",
                    ]
                )
                if dynamic_one_dimensional_memory:
                    lines.append(
                        f"      {target}[first_index + offset] = "
                        f"{assigned_block_value};"
                    )
                else:
                    lines.append("      case (first_index + offset)")
                    dimension = signal.unpacked[0]
                    for linear, element_target, _ in memory_targets:
                        actual_index = dimension.low + linear
                        lines.append(
                            f"        {actual_index}: {element_target} = "
                            f"{assigned_block_value};"
                        )
                    lines.extend(
                        [
                            '        default: $fatal(1, "block deposit index %0d is out of bounds", first_index + offset);',
                            "      endcase",
                        ]
                    )
                lines.extend(["    end", "  endfunction", ""])

        if (path, "deposit_logic") in operations:
            name = hierarchy_export_name(manifest, index, "deposit_logic")
            assigned_value = hierarchy_assignment_value(signal, "value")
            lines.extend(
                [
                    f'  export "DPI-C" function {name};',
                    f"  function void {name}(input int index, "
                    f"input {logic_value_type} value);",
                ]
            )
            if dynamic_one_dimensional_memory:
                lines.append(f"    {target}[index] = {assigned_value};")
            elif len(signal.unpacked) == 1:
                lines.append("    case (index)")
                dimension = signal.unpacked[0]
                for linear, element_target, _ in memory_targets:
                    actual_index = dimension.low + linear
                    lines.append(
                        f"      {actual_index}: {element_target} = "
                        f"{assigned_value};"
                    )
                lines.extend(
                    [
                        f'      default: $fatal(1, "logic deposit index %0d is out of bounds", index);',
                        "    endcase",
                    ]
                )
            elif signal.unpacked:
                lines.append("    case (index)")
                for linear, element_target, _ in memory_targets:
                    lines.append(
                        f"      {linear}: {element_target} = "
                        f"{assigned_value};"
                    )
                lines.extend(
                    [
                        f'      default: $fatal(1, "logic deposit index %0d is out of bounds", index);',
                        "    endcase",
                    ]
                )
            else:
                lines.append(f"    {target} = {assigned_value};")
            lines.extend(["  endfunction", ""])

        if (path, "force") in operations:
            name = hierarchy_export_name(manifest, index, "force")
            lines.extend(
                [
                    f'  export "DPI-C" function {name};',
                    f"  function void {name}(input int index, "
                    f"input {value_type} value);",
                ]
            )
            if len(signal.unpacked) == 1:
                lines.append("    case (index)")
                dimension = signal.unpacked[0]
                for linear, element_target, _ in memory_targets:
                    actual_index = dimension.low + linear
                    shadow = f"{force_shadow}[{actual_index}]"
                    forced_value = hierarchy_assignment_value(signal, shadow)
                    lines.extend(
                        [
                            f"      {actual_index}: begin",
                            f"        {shadow} = value;",
                            f"        force {element_target} = "
                            f"{forced_value};",
                            "      end",
                        ]
                    )
                lines.extend(
                    [
                        f'      default: $fatal(1, "force index %0d is out of bounds", index);',
                        "    endcase",
                    ]
                )
            elif signal.unpacked:
                lines.append("    case (index)")
                for linear, element_target, shadow_indices in memory_targets:
                    forced_value = hierarchy_assignment_value(
                        signal, f"{force_shadow}{shadow_indices}"
                    )
                    lines.extend(
                        [
                            f"      {linear}: begin",
                            f"        {force_shadow}{shadow_indices} = value;",
                            f"        force {element_target} = "
                            f"{forced_value};",
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
                forced_value = hierarchy_assignment_value(
                    signal, force_shadow
                )
                lines.extend(
                    [
                        f"    {force_shadow} = value;",
                        f"    force {target} = {forced_value};",
                    ]
                )
            lines.extend(["  endfunction", ""])

        if (path, "force_logic") in operations:
            name = hierarchy_export_name(manifest, index, "force_logic")
            lines.extend(
                [
                    f'  export "DPI-C" function {name};',
                    f"  function void {name}(input int index, "
                    f"input {logic_value_type} value);",
                ]
            )
            if len(signal.unpacked) == 1:
                lines.append("    case (index)")
                dimension = signal.unpacked[0]
                for linear, element_target, _ in memory_targets:
                    actual_index = dimension.low + linear
                    shadow = f"{force_shadow}[{actual_index}]"
                    forced_value = hierarchy_assignment_value(signal, shadow)
                    lines.extend(
                        [
                            f"      {actual_index}: begin",
                            f"        {shadow} = value;",
                            f"        force {element_target} = "
                            f"{forced_value};",
                            "      end",
                        ]
                    )
                lines.extend(
                    [
                        f'      default: $fatal(1, "logic force index %0d is out of bounds", index);',
                        "    endcase",
                    ]
                )
            elif signal.unpacked:
                lines.append("    case (index)")
                for linear, element_target, shadow_indices in memory_targets:
                    shadow = f"{force_shadow}{shadow_indices}"
                    forced_value = hierarchy_assignment_value(signal, shadow)
                    lines.extend(
                        [
                            f"      {linear}: begin",
                            f"        {shadow} = value;",
                            f"        force {element_target} = "
                            f"{forced_value};",
                            "      end",
                        ]
                    )
                lines.extend(
                    [
                        f'      default: $fatal(1, "logic force index %0d is out of bounds", index);',
                        "    endcase",
                    ]
                )
            else:
                forced_value = hierarchy_assignment_value(
                    signal, force_shadow
                )
                lines.extend(
                    [
                        f"    {force_shadow} = value;",
                        f"    force {target} = {forced_value};",
                    ]
                )
            lines.extend(["  endfunction", ""])

        if (path, "release") in operations:
            name = hierarchy_export_name(manifest, index, "release")
            lines.extend(
                [
                    f'  export "DPI-C" function {name};',
                    f"  function void {name}(input int index);",
                ]
            )
            if len(signal.unpacked) == 1:
                lines.append("    case (index)")
                dimension = signal.unpacked[0]
                for linear, element_target, _ in memory_targets:
                    actual_index = dimension.low + linear
                    lines.append(
                        f"      {actual_index}: release {element_target};"
                    )
                lines.extend(
                    [
                        f'      default: $fatal(1, "release index %0d is out of bounds", index);',
                        "    endcase",
                    ]
                )
            elif signal.unpacked:
                lines.append("    case (index)")
                for linear, element_target, _ in memory_targets:
                    lines.append(
                        f"      {linear}: release {element_target};"
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


def sv_port_export_functions(
    ports: list[Port], manifest: dict[str, Any]
) -> list[str]:
    lines: list[str] = []
    for index, port in enumerate(ports):
        if port.transport != "on_demand":
            continue
        index_formals = ", ".join(
            f"input int index_{rank}" for rank in range(len(port.unpacked))
        )
        target = sv_port_reference(
            port,
            [f"index_{rank}" for rank in range(len(port.unpacked))],
        )
        value_type = (
            "int unsigned"
            if port.width <= 32
            else (
                "longint unsigned"
                if port.width <= 64
                else f"bit [{port.width - 1}:0]"
            )
        )
        get_name = port_export_name(manifest, index, "get")
        lines.append(f'  export "DPI-C" function {get_name};')
        if port.width <= 64:
            lines.extend(
                [
                    f"  function {value_type} {get_name}({index_formals});",
                    f"    {get_name} = $unsigned({target});",
                    "  endfunction",
                    "",
                ]
            )
        else:
            separator = ", " if index_formals else ""
            lines.extend(
                [
                    f"  function void {get_name}({index_formals}{separator}"
                    f"output {value_type} value);",
                    f"    value = {target};",
                    "  endfunction",
                    "",
                ]
            )

        if port.direction != "input":
            continue
        set_name = port_export_name(manifest, index, "set")
        separator = ", " if index_formals else ""
        lines.extend(
            [
                f'  export "DPI-C" function {set_name};',
                f"  function void {set_name}({index_formals}{separator}"
                f"input {value_type} value);",
                f"    {target} = value;",
                "  endfunction",
                "",
            ]
        )
    return lines


def sv_inout_export_functions(
    ports: list[Port], manifest: dict[str, Any]
) -> list[str]:
    lines: list[str] = []
    for port_index, port in enumerate(ports):
        if port.direction != "inout":
            continue
        value_type = (
            "int unsigned"
            if port.width <= 32
            else (
                "longint unsigned"
                if port.width <= 64
                else f"bit [{port.width - 1}:0]"
            )
        )
        drive_name = inout_export_name(manifest, port_index, "drive")
        high_z_name = inout_export_name(manifest, port_index, "high_z")
        drive_storage = sv_inout_storage_name(port, "drive")
        enable_storage = sv_inout_storage_name(port, "oe")
        indexed = list(enumerate(sv_index_tuples(port)))

        lines.extend(
            [
                f'  export "DPI-C" function {drive_name};',
                f"  function void {drive_name}(",
                "      input int index, input " + value_type + " value);",
            ]
        )
        if port.unpacked:
            lines.append("    case (index)")
            for linear, indices in indexed:
                suffix = "".join(f"[{item}]" for item in indices)
                lines.extend(
                    [
                        f"      {linear}: begin",
                        f"        {drive_storage}{suffix} = value;",
                        f"        {enable_storage}{suffix} = 1'b1;",
                        "      end",
                    ]
                )
            lines.extend(
                [
                    f'      default: $fatal(1, "inout {port.name} index %0d '
                    'is out of bounds", index);',
                    "    endcase",
                ]
            )
        else:
            lines.extend(
                [
                    f"    {drive_storage} = value;",
                    f"    {enable_storage} = 1'b1;",
                ]
            )
        lines.extend(
            [
                "  endfunction",
                "",
                f'  export "DPI-C" function {high_z_name};',
                f"  function void {high_z_name}(input int index);",
            ]
        )
        if port.unpacked:
            lines.append("    case (index)")
            for linear, indices in indexed:
                suffix = "".join(f"[{item}]" for item in indices)
                lines.append(
                    f"      {linear}: {enable_storage}{suffix} = 1'b0;"
                )
            lines.extend(
                [
                    f'      default: $fatal(1, "inout {port.name} index %0d '
                    'is out of bounds", index);',
                    "    endcase",
                ]
            )
        else:
            lines.append(f"    {enable_storage} = 1'b0;")
        lines.extend(["  endfunction", ""])
    return lines


def render_sv(
    ports: list[Port],
    internals: list[Internal],
    manifest: dict[str, Any],
    source: str,
    hierarchy: HierarchyCatalog | None = None,
    interfaces: tuple[InterfacePort, ...] = (),
) -> str:
    hierarchy = hierarchy or HierarchyCatalog()
    clocks = clock_configs(manifest)
    edge_observers = edge_observer_ports(ports, manifest)
    hierarchy_edge_paths_selected = hierarchy_edge_paths(
        list(manifest.get("hierarchy_accesses", []))
    )
    hierarchy_signals = {
        signal.hdl_path: signal for signal in hierarchy.signals
    }
    dynamic_clocks = bool(manifest.get("run", {}).get("dynamic_clocks", False))
    driven_names = driven_port_names(ports, manifest)
    observed_names = observed_port_names(ports, manifest)
    clock_owned = clock_owned_element_indices(
        ports, manifest, {"generated", "registered"}
    )
    run = manifest.get("run", {})
    compact_input_transport = bool(run.get("compact_input_transport", True))
    observed_ports = [port for port in ports if port.name in observed_names]
    driven_ports = [port for port in ports if port.name in driven_names]
    dynamic_clock_ports = [
        port
        for port in driven_ports
        if port.width == 1 and not port.unpacked
    ]
    selected_on_demand = on_demand_ports(ports)
    packed_observed_ports = packed_ports(observed_ports)
    packed_driven_ports = packed_ports(driven_ports)
    effective_compact_input_transport = (
        compact_input_transport
        or bool(selected_on_demand)
        or static_binding_enabled(manifest)
    )
    all_offsets = signal_word_offsets(ports)
    global_offsets = {
        port.name: offset for port, offset in zip(ports, all_offsets)
    }
    input_offsets = (
        directional_transport_offsets(packed_observed_ports)
        if effective_compact_input_transport
        else {
            port.name: global_offsets[port.name]
            for port in packed_observed_ports
        }
    )
    output_offsets = directional_transport_offsets(packed_driven_ports)
    input_word_count = sum(
        port_word_count(port) for port in packed_observed_ports
    )
    output_word_count = sum(port_word_count(port) for port in packed_driven_ports)
    input_storage_count = max(
        1,
        input_word_count
        if effective_compact_input_transport
        else all_offsets[-1],
    )
    output_storage_count = max(1, output_word_count)
    primary_clocks = [clock for clock in clocks if clock.get("primary", False)]
    if len(primary_clocks) > 1:
        raise CodegenError("only one clock may be marked primary")
    primary_clock = primary_clocks[0] if primary_clocks else (clocks[0] if clocks else None)
    calendar_clocks = [
        clock
        for clock in clocks
        if clock_source(clock) in {"generated", "registered"}
    ]
    calendar_clock_names = {clock["port"] for clock in calendar_clocks}
    edge_interest_entries: dict[str, bool] = {
        sv_signal_constant(port): False for port in edge_observers
    }
    edge_interest_entries.update(
        {
            sv_clock_signal_expression(clock, ports): True
            for clock in clocks
        }
    )
    calendar_dynamic_ports = [
        port
        for port in dynamic_clock_ports
        if dynamic_clocks
        and not calendar_clocks
        and port.name not in calendar_clock_names
    ]
    calendar_clock_count = len(calendar_clocks) + len(calendar_dynamic_ports)
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
    clock_config_function = run.get(
        "clock_config_function", "cpptb_dpi_clock_config"
    )
    phase_dispatch_function = run.get(
        "phase_dispatch_function", "cpptb_dpi_phase_dispatch"
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
            "  localparam int PHASE_READ_WRITE = 5;",
            "  localparam int PHASE_READ_ONLY = 6;",
            "  localparam int PHASE_NEXT_TIME_STEP = 7;",
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
            "  localparam int STEP_NEXT_TICK_TIMER = 128;",
            "  localparam int STEP_TIMER_IDLE = 256;",
            "  localparam int STEP_READ_WRITE = 512;",
            "  localparam int STEP_READ_ONLY = 1024;",
            "  localparam int STEP_NEXT_TIME_STEP = 2048;",
            "  localparam int STEP_PHASE_MASK = STEP_READ_WRITE |",
            "      STEP_READ_ONLY | STEP_NEXT_TIME_STEP;",
            "",
        ]
    )
    for port, offset in zip(ports, offsets):
        lines.append(f"  localparam int {sv_signal_constant(port)} = {offset};")
    for index, path in enumerate(hierarchy_edge_paths_selected):
        lines.append(
            f"  localparam int "
            f"{sv_hierarchy_edge_constant(path)} "
            f"= {offsets[-1] + index};"
        )
    for port in packed_observed_ports:
        lines.append(
            f"  localparam int INPUT_{sv_signal_constant(port)} = "
            f"{input_offsets[port.name]};"
        )
    for port in packed_driven_ports:
        lines.append(
            f"  localparam int OUTPUT_{sv_signal_constant(port)} = "
            f"{output_offsets[port.name]};"
        )
    lines.extend(
        [
            f"  localparam int CPPTB_SIGNAL_COUNT = "
            f"{offsets[-1] + len(hierarchy_edge_paths_selected)};",
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
            f'  import "DPI-C" function longint unsigned {next_deadline_function}();',
            f'  import "DPI-C" function int unsigned {edge_interest_function}(',
            "      input int unsigned signal_id",
            "  );",
        ]
    )
    if dynamic_clocks:
        lines.extend(
            [
                f'  import "DPI-C" function longint unsigned {clock_config_function}(',
                "      input int unsigned signal_id,",
                "      input int unsigned field",
                "  );",
            ]
        )
    lines.append("")
    lines.extend(sv_decl(port) for port in ports if port.interface_name is None)
    for interface in interfaces:
        lines.extend(sv_interface_declarations(interface))
    for port in ports:
        if port.direction == "inout":
            lines.extend(sv_inout_declarations(port))
    lines.extend(
        [
            "",
            "  int unsigned iterations;",
            "  longint unsigned sim_cycles;",
            "  longint unsigned timer_generation;",
            "  longint unsigned timer_deadline;",
            "  longint unsigned timer_owner_target;",
            "  event timer_kick;",
            "  event phase_outputs_pending;",
            "`ifdef CPPTB_SV_DPI_TIMING",
            "  event phase_kick;",
            "  event phase_barrier_kick;",
            "  event time_step_started;",
            "  int phase_requests;",
            "  bit phase_barrier_token;",
            "  time last_started_time;",
            "`endif",
            "`ifdef CPPTB_SV_DPI_CALENDAR_TIMING",
            f"  localparam int CALENDAR_CLOCK_COUNT = {calendar_clock_count};",
            "  localparam int CALENDAR_CLOCK_STORAGE =",
            "      CALENDAR_CLOCK_COUNT > 0 ? CALENDAR_CLOCK_COUNT : 1;",
            "  longint unsigned calendar_clock_half_period[0:CALENDAR_CLOCK_STORAGE-1];",
            "  longint unsigned calendar_clock_next_edge[0:CALENDAR_CLOCK_STORAGE-1];",
            "  bit calendar_clock_active[0:CALENDAR_CLOCK_STORAGE-1];",
            "`endif",
            "  int status;",
            "  bit track_falling_edges;",
            "  bit clock_drivers_active;",
            "  int initial_requests;",
            "  int unsigned in_words[0:INPUT_WORD_COUNT-1];",
            "  int unsigned out_words[0:OUTPUT_WORD_COUNT-1];",
            "  int unsigned edge_interest[0:CPPTB_SIGNAL_COUNT-1];",
            "  bit registered_clock[0:CPPTB_SIGNAL_COUNT-1];",
            "  bit primary_clock[0:CPPTB_SIGNAL_COUNT-1];",
        ]
    )
    lines.extend(["", "  task automatic pack_inputs();"])
    for port in packed_observed_ports:
        lines.extend(
            sv_pack_assignments(port, f"INPUT_{sv_signal_constant(port)}")
        )
    lines.extend(["  endtask", "", "  task automatic apply_outputs();"])
    for port in packed_driven_ports:
        assignments = sv_output_assignments(
            port,
            f"OUTPUT_{sv_signal_constant(port)}",
            clock_owned.get(port.name),
        )
        if dynamic_clocks and port in dynamic_clock_ports:
            constant = sv_signal_constant(port)
            lines.append(
                f"    if (!clock_drivers_active || !registered_clock[{constant}]) begin"
            )
            lines.extend(f"  {line}" for line in assignments)
            lines.append("    end")
        else:
            lines.extend(assignments)
    lines.extend(
        [
            "  endtask",
            "",
            "  always @(phase_outputs_pending) begin",
            "    apply_outputs();",
            "  end",
            "`ifdef CPPTB_SV_DPI_NBA_TIMING",
            "  always @(phase_barrier_kick) begin",
            "    phase_barrier_token <= ~phase_barrier_token;",
            "  end",
            "`endif",
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
            "  task automatic run_phase_step(",
            "      input int unsigned phase,",
            "      output int requests",
            "  );",
            "    pack_inputs();",
            f"    requests = {step_function}(phase, $time, sim_cycles,",
            "                               NO_SIGNAL, EDGE_RISING,",
            "                               in_words);",
            "    if ((requests >= 0) &&",
            "        ((requests & STEP_OUTPUTS_CHANGED) != 0)) begin",
            f"      {pull_outputs_function}(out_words);",
            "`ifdef CPPTB_SV_DPI_TIMING",
            "      apply_outputs();",
            "`else",
            "      -> phase_outputs_pending;",
            "`endif",
            "    end",
            "    update_status(requests);",
            "  endtask",
            "",
            "`ifdef CPPTB_SV_DPI_TIMING",
            "  task automatic note_time_step();",
            "    int requests;",
            "    if ($time != last_started_time) begin",
            "      last_started_time = $time;",
            "`ifdef CPPTB_SV_DPI_CALENDAR_TIMING",
            "      if ((phase_requests & STEP_NEXT_TIME_STEP) != 0) begin",
            "        phase_requests &= ~STEP_NEXT_TIME_STEP;",
            "        run_phase_step(PHASE_NEXT_TIME_STEP, requests);",
            "        service_requests(requests);",
            "      end",
            "`else",
            "      -> time_step_started;",
            "`endif",
            "    end",
            "  endtask",
            "",
            "  task automatic phase_settle_barrier();",
            "`ifdef CPPTB_SV_DPI_NBA_TIMING",
            "    -> phase_barrier_kick;",
            "    @(phase_barrier_token);",
            "`endif",
            "  endtask",
            "",
            "`ifndef CPPTB_SV_DPI_CALENDAR_TIMING",
            "  task automatic phase_pump();",
            "    int requests;",
            "    time requested_time;",
            "    while (status == 0) begin",
            "      if ((phase_requests & STEP_PHASE_MASK) == 0) begin",
            "        @(phase_kick);",
            "      end",
            "      if ((phase_requests & STEP_READ_WRITE) != 0) begin",
            "        phase_requests &= ~STEP_READ_WRITE;",
            "        phase_settle_barrier();",
            "        run_phase_step(PHASE_READ_WRITE, requests);",
            "        service_requests(requests);",
            "      end",
            "      if ((phase_requests & STEP_READ_ONLY) != 0) begin",
            "        phase_requests &= ~STEP_READ_ONLY;",
            "        phase_settle_barrier();",
            "        run_phase_step(PHASE_READ_ONLY, requests);",
            "        service_requests(requests);",
            "      end",
            "      if ((phase_requests & STEP_NEXT_TIME_STEP) != 0) begin",
            "        phase_requests &= ~STEP_NEXT_TIME_STEP;",
            "        requested_time = $time;",
            "        while (last_started_time <= requested_time) begin",
            "          @(time_step_started);",
            "        end",
            "        run_phase_step(PHASE_NEXT_TIME_STEP, requests);",
            "        service_requests(requests);",
            "      end",
            "    end",
            "  endtask",
            "`endif",
            "`endif",
            "",
            "  task automatic timer_wakeup(",
            "      input longint unsigned deadline,",
            "      input longint unsigned generation",
            "  );",
            "    int requests;",
            "    if (deadline > $time) begin",
            "      #(deadline - $time);",
            "    end",
            "`ifdef CPPTB_SV_DPI_TIMING",
            "    note_time_step();",
            "`endif",
            "    if ((status == 0) && (generation == timer_generation)) begin",
            "      run_step(PHASE_DELAY, NO_SIGNAL, EDGE_RISING, requests);",
            "      service_requests(requests);",
            "`ifdef CPPTB_SV_DPI_CALENDAR_TIMING",
            "      calendar_drain_phases();",
            "`endif",
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
            "`ifdef CPPTB_SV_DPI_TIMING",
            "        note_time_step();",
            "`endif",
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
            "    longint unsigned generation;",
            "    requests = initial_requests;",
        ]
    )
    if edge_interest_entries or hierarchy_edge_paths_selected:
        lines.extend(
            [
                "    if ((requests & STEP_EDGE_INTEREST_CHANGED) != 0) begin",
            ]
        )
        for constant, clock_interest in edge_interest_entries.items():
            assignment = "|=" if clock_interest else "="
            lines.append(
                f"      edge_interest[{constant}] {assignment} "
                f"{edge_interest_function}({constant});"
            )
        for path in hierarchy_edge_paths_selected:
            constant = sv_hierarchy_edge_constant(path)
            lines.append(
                f"      edge_interest[{constant}] = "
                f"{edge_interest_function}({constant});"
            )
        lines.append("    end")
    lines.extend(
        [
            "    if ((requests & STEP_TIMER_IDLE) != 0) begin",
            "      timer_deadline = NO_TIMER;",
            "      timer_generation++;",
            "    end",
            "    if ((requests & STEP_NEXT_TICK_TIMER) != 0) begin",
            "      timer_deadline = $time + 1;",
            "      timer_generation++;",
            "      generation = timer_generation;",
            "      if (timer_owner_target == NO_TIMER) begin",
            "        -> timer_kick;",
            "      end else if (timer_deadline < timer_owner_target) begin",
            "        fork",
            "          timer_wakeup(timer_deadline, generation);",
            "        join_none",
            "      end",
            "    end else if ((requests & STEP_TIMER_CHANGED) != 0) begin",
            "      update_timer_schedule();",
            "    end",
            "`ifdef CPPTB_SV_DPI_TIMING",
            "    if ((requests & STEP_PHASE_MASK) != 0) begin",
            "      phase_requests |= requests & STEP_PHASE_MASK;",
            "`ifndef CPPTB_SV_DPI_CALENDAR_TIMING",
            "      -> phase_kick;",
            "`endif",
            "    end",
            "`endif",
            "  endtask",
            "",
        ]
    )

    lines.extend(
        [
            "`ifdef CPPTB_SV_DPI_CALENDAR_TIMING",
            "  task automatic calendar_initialize();",
            "    for (int i = 0; i < CALENDAR_CLOCK_STORAGE; i++) begin",
            "      calendar_clock_active[i] = 1'b0;",
            "      calendar_clock_half_period[i] = 0;",
            "      calendar_clock_next_edge[i] = NO_TIMER;",
            "    end",
        ]
    )
    for index, clock in enumerate(calendar_clocks):
        half_period = clock.get("half_period", "1ns")
        phase = clock.get("phase")
        lines.extend(
            [
                f"    calendar_clock_active[{index}] = 1'b1;",
                f"    calendar_clock_half_period[{index}] = {half_period};",
            ]
        )
        if phase:
            lines.append(
                f"    calendar_clock_next_edge[{index}] = "
                f"$time + time'({phase}) + calendar_clock_half_period[{index}];"
            )
        else:
            lines.append(
                f"    calendar_clock_next_edge[{index}] = "
                f"$time + calendar_clock_half_period[{index}];"
            )
    for dynamic_index, port in enumerate(calendar_dynamic_ports):
        index = len(calendar_clocks) + dynamic_index
        constant = sv_signal_constant(port)
        lines.extend(
            [
                f"    calendar_clock_half_period[{index}] =",
                f"        {clock_config_function}({constant}, 0);",
                f"    calendar_clock_active[{index}] =",
                f"        calendar_clock_half_period[{index}] != 0;",
                f"    calendar_clock_next_edge[{index}] = $time +",
                f"        {clock_config_function}({constant}, 1) +",
                f"        calendar_clock_half_period[{index}];",
            ]
        )
    lines.extend(
        [
            "  endtask",
            "",
            "  task automatic calendar_drain_phases();",
            "    int requests;",
            "    while ((status == 0) &&",
            "           ((phase_requests & (STEP_READ_WRITE | STEP_READ_ONLY)) != 0)) begin",
            "      if ((phase_requests & STEP_READ_WRITE) != 0) begin",
            "        phase_requests &= ~STEP_READ_WRITE;",
            "        phase_settle_barrier();",
            "        run_phase_step(PHASE_READ_WRITE, requests);",
            "        service_requests(requests);",
            "      end else begin",
            "        phase_requests &= ~STEP_READ_ONLY;",
            "        phase_settle_barrier();",
            "        run_phase_step(PHASE_READ_ONLY, requests);",
            "        service_requests(requests);",
            "      end",
            "    end",
            "  endtask",
            "",
            "  task automatic calendar_next_target(",
            "      output longint unsigned target",
            "  );",
            "    target = timer_deadline;",
            "    for (int i = 0; i < CALENDAR_CLOCK_COUNT; i++) begin",
            "      if (calendar_clock_active[i] &&",
            "          (calendar_clock_next_edge[i] < target)) begin",
            "        target = calendar_clock_next_edge[i];",
            "      end",
            "    end",
            "  endtask",
            "",
            "  task automatic calendar_drive_due_clocks();",
            "    int requests;",
            "    int event_edge;",
        ]
    )
    for index, clock in enumerate(calendar_clocks):
        clock_name = clock["port"]
        port, clock_indices = resolve_clock_port(clock, ports)
        clock_target = sv_port_reference(
            port, [str(item) for item in clock_indices]
        )
        constant = sv_clock_signal_expression(clock, ports)
        primary_literal = (
            "1'b1"
            if primary_clock is not None
            and same_clock(clock, primary_clock)
            else "1'b0"
        )
        lines.extend(
            [
                f"    if (calendar_clock_active[{index}] &&",
                f"        (calendar_clock_next_edge[{index}] <= $time)) begin",
                f"      {clock_target} = ~{clock_target};",
                f"      calendar_clock_next_edge[{index}] +=",
                f"          calendar_clock_half_period[{index}];",
                f"      event_edge = {clock_target} ? EDGE_RISING : EDGE_FALLING;",
            ]
        )
        if primary_clock is not None and same_clock(clock, primary_clock):
            lines.extend(
                [
                    "      if (event_edge == EDGE_RISING) begin",
                    "        sim_cycles++;",
                    "      end",
                ]
            )
        lines.extend(
            [
                "      if (",
                f"          ((event_edge == EDGE_RISING) && ((edge_interest[{constant}] & 1) != 0)) ||",
                f"          ((event_edge == EDGE_FALLING) && ((edge_interest[{constant}] & 2) != 0)) ||",
                "          ((event_edge == EDGE_RISING) &&",
                f"           {primary_literal} &&",
                "           (timer_deadline == NO_TIMER))) begin",
                f"        run_step(PHASE_EDGE, {constant}, event_edge, requests);",
                "        service_requests(requests);",
                "      end",
                "    end",
            ]
        )
    for dynamic_index, port in enumerate(calendar_dynamic_ports):
        index = len(calendar_clocks) + dynamic_index
        constant = sv_signal_constant(port)
        clock_target = sv_scalar_port_reference(port)
        lines.extend(
            [
                f"    if (calendar_clock_active[{index}] &&",
                f"        (calendar_clock_next_edge[{index}] <= $time)) begin",
                f"      {clock_target} = ~{clock_target};",
                f"      calendar_clock_next_edge[{index}] +=",
                f"          calendar_clock_half_period[{index}];",
                f"      event_edge = {clock_target} ? EDGE_RISING : EDGE_FALLING;",
                f"      if (primary_clock[{constant}] &&",
                "          (event_edge == EDGE_RISING)) begin",
                "        sim_cycles++;",
                "      end",
                "      if (",
                f"          ((event_edge == EDGE_RISING) && ((edge_interest[{constant}] & 1) != 0)) ||",
                f"          ((event_edge == EDGE_FALLING) && ((edge_interest[{constant}] & 2) != 0)) ||",
                f"          (primary_clock[{constant}] &&",
                "           (event_edge == EDGE_RISING) &&",
                "           (timer_deadline == NO_TIMER))) begin",
                f"        run_step(PHASE_EDGE, {constant}, event_edge, requests);",
                "        service_requests(requests);",
                "      end",
                "    end",
            ]
        )
    lines.extend(
        [
            "  endtask",
            "",
            "  task automatic calendar_owner();",
            "    int requests;",
            "    longint unsigned target;",
            "    calendar_drain_phases();",
            "    while (status == 0) begin",
            "      calendar_next_target(target);",
            "      if (target == NO_TIMER) begin",
            "        timer_owner_target = NO_TIMER;",
            "        @(timer_kick);",
            "      end else begin",
            "        timer_owner_target = target;",
            "        if (target > $time) begin",
            "          #(target - $time);",
            "        end",
            "        timer_owner_target = NO_TIMER;",
            "        if (status == 0) begin",
            "          note_time_step();",
            "          if ((timer_deadline != NO_TIMER) &&",
            "              (timer_deadline <= $time)) begin",
            "            run_step(PHASE_DELAY, NO_SIGNAL, EDGE_RISING, requests);",
            "            service_requests(requests);",
            "          end",
            "          calendar_drive_due_clocks();",
            "          calendar_drain_phases();",
            "        end",
            "      end",
            "    end",
            "  endtask",
            "`endif",
            "",
        ]
    )

    lines.extend(
        [
            f"  task automatic {phase_dispatch_function}(",
            "      input int unsigned phase",
            "  );",
            "    int requests;",
            "    run_phase_step(phase, requests);",
            "    service_requests(requests);",
            "  endtask",
            f'  export "DPI-C" task {phase_dispatch_function};',
            "",
        ]
    )

    for index, clock in enumerate(clocks):
        clock_name = clock["port"]
        port, clock_indices = resolve_clock_port(clock, ports)
        clock_target = sv_port_reference(
            port, [str(item) for item in clock_indices]
        )
        constant = sv_clock_signal_expression(clock, ports)
        primary_literal = (
            "1'b1"
            if primary_clock is not None
            and same_clock(clock, primary_clock)
            else "1'b0"
        )
        if clock_source(clock) in {"generated", "registered"}:
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
                    "`ifdef CPPTB_SV_DPI_TIMING",
                    "        note_time_step();",
                    "`endif",
                    f"        {clock_target} = ~{clock_target};",
                    f"        next_edge = next_edge + {clock.get('half_period', '1ns')};",
                    f"        event_edge = {clock_target} ? EDGE_RISING : EDGE_FALLING;",
                ]
            )
            if primary_clock is not None and same_clock(clock, primary_clock):
                lines.extend(
                    [
                        "        if (event_edge == EDGE_RISING) begin",
                        "          sim_cycles++;",
                        "        end",
                    ]
                )
            lines.extend(
                [
                    "        if (",
                    f"            ((event_edge == EDGE_RISING) && ((edge_interest[{constant}] & 1) != 0)) ||",
                    f"            ((event_edge == EDGE_FALLING) && ((edge_interest[{constant}] & 2) != 0)) ||",
                    "            ((event_edge == EDGE_RISING) &&",
                    f"             {primary_literal} &&",
                    "             (timer_deadline == NO_TIMER))) begin",
                    f"          run_step(PHASE_EDGE, {constant},",
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
                f"      @({clock_target});",
                "`ifdef CPPTB_SV_DPI_TIMING",
                "      note_time_step();",
                "`endif",
                f"      event_edge = {clock_target} ? EDGE_RISING : EDGE_FALLING;",
            ]
        )
        if primary_clock is not None and same_clock(clock, primary_clock):
            lines.extend(
                [
                    "      if (event_edge == EDGE_RISING) begin",
                    "        sim_cycles++;",
                    "      end",
                ]
            )
        lines.extend(
            [
                "      if (",
                f"          ((event_edge == EDGE_RISING) && ((edge_interest[{constant}] & 1) != 0)) ||",
                f"          ((event_edge == EDGE_FALLING) && ((edge_interest[{constant}] & 2) != 0)) ||",
                "          ((event_edge == EDGE_RISING) &&",
                f"           {primary_literal} &&",
                "           (timer_deadline == NO_TIMER))) begin",
                "        if (status == 0) begin",
                f"          run_step(PHASE_EDGE, {constant},",
                "                   event_edge, requests);",
                "          service_requests(requests);",
                "`ifdef CPPTB_SV_DPI_CALENDAR_TIMING",
                "          calendar_drain_phases();",
                "`endif",
                "        end",
                "      end",
                "    end",
                "  endtask",
                "",
            ]
        )

    if dynamic_clocks:
        for index, port in enumerate(dynamic_clock_ports):
            constant = sv_signal_constant(port)
            clock_target = sv_scalar_port_reference(port)
            lines.extend(
                [
                    f"  task automatic drive_registered_clock_{index}();",
                    "    int requests;",
                    "    int event_edge;",
                    "    longint unsigned half_period;",
                    "    longint unsigned phase;",
                    f"    half_period = {clock_config_function}({constant}, 0);",
                    "    if (half_period != 0) begin",
                    f"      phase = {clock_config_function}({constant}, 1);",
                    "      if (phase != 0) begin",
                    "        #(phase);",
                    "      end",
                    "      while (status == 0) begin",
                    "        #(half_period);",
                    "        if (status == 0) begin",
                    "`ifdef CPPTB_SV_DPI_TIMING",
                    "          note_time_step();",
                    "`endif",
                    f"          {clock_target} = ~{clock_target};",
                    f"          event_edge = {clock_target} ? EDGE_RISING : EDGE_FALLING;",
                    f"          if (primary_clock[{constant}] &&",
                    "              (event_edge == EDGE_RISING)) begin",
                    "            sim_cycles++;",
                    "          end",
                    "          if (",
                    f"              ((event_edge == EDGE_RISING) && ((edge_interest[{constant}] & 1) != 0)) ||",
                    f"              ((event_edge == EDGE_FALLING) && ((edge_interest[{constant}] & 2) != 0)) ||",
                    f"              (primary_clock[{constant}] &&",
                    "               (event_edge == EDGE_RISING) &&",
                    "               (timer_deadline == NO_TIMER))) begin",
                    f"            run_step(PHASE_EDGE, {constant}, event_edge, requests);",
                    "            service_requests(requests);",
                    "          end",
                    "        end",
                    "      end",
                    "    end",
                    "  endtask",
                    "",
                ]
            )

    for index, port in enumerate(edge_observers):
        constant = sv_signal_constant(port)
        signal_target = sv_scalar_port_reference(port)
        lines.extend(
            [
                f"  task automatic observe_signal_{index}();",
                "    int requests;",
                "    int event_edge;",
                "    while (status == 0) begin",
                f"      @({signal_target});",
                "`ifdef CPPTB_SV_DPI_TIMING",
                "      note_time_step();",
                "`endif",
                "      if (status == 0) begin",
                f"        event_edge = {signal_target} ? EDGE_RISING : EDGE_FALLING;",
                "        if (",
                f"            ((event_edge == EDGE_RISING) && ((edge_interest[{constant}] & 1) != 0)) ||",
                f"            ((event_edge == EDGE_FALLING) && ((edge_interest[{constant}] & 2) != 0))) begin",
                f"          run_step(PHASE_EDGE, {constant}, event_edge, requests);",
                "          service_requests(requests);",
                "`ifdef CPPTB_SV_DPI_CALENDAR_TIMING",
                "          calendar_drain_phases();",
                "`endif",
                "        end",
                "      end",
                "    end",
                "  endtask",
                "",
            ]
        )

    for index, path in enumerate(hierarchy_edge_paths_selected):
        constant = sv_hierarchy_edge_constant(path)
        target = f"i_dut.{path}"
        lines.extend(
            [
                f"  task automatic observe_hierarchy_signal_{index}();",
                "    int requests;",
                "    int event_edge;",
                "    while (status == 0) begin",
                f"      @({target});",
                "`ifdef CPPTB_SV_DPI_TIMING",
                "      note_time_step();",
                "`endif",
                "      if (status == 0) begin",
                f"        event_edge = {target} ? EDGE_RISING : EDGE_FALLING;",
                "        if (",
                f"            ((event_edge == EDGE_RISING) && ((edge_interest[{constant}] & 1) != 0)) ||",
                f"            ((event_edge == EDGE_FALLING) && ((edge_interest[{constant}] & 2) != 0))) begin",
                f"          run_step(PHASE_EDGE, {constant}, event_edge, requests);",
                "          service_requests(requests);",
                "`ifdef CPPTB_SV_DPI_CALENDAR_TIMING",
                "          calendar_drain_phases();",
                "`endif",
                "        end",
                "      end",
                "    end",
                "  endtask",
                "",
            ]
        )

    lines.append("  initial begin")
    for clock in clocks:
        source_kind = clock_source(clock)
        clock_port, clock_indices = resolve_clock_port(clock, ports)
        clock_target = sv_port_reference(
            clock_port, [str(item) for item in clock_indices]
        )
        if source_kind == "generated":
            lines.append(f"    {clock_target} = 1'b0;")
        elif source_kind == "registered":
            lines.append(
                f"    {clock_target} = "
                f"1'b{int(clock.get('initial_value', 0))};"
            )
    for port in ports:
        if port.direction == "input":
            lines.extend(
                sv_zero_assignments(
                    port,
                    skip_linear_indices=clock_owned.get(port.name),
                )
            )
        elif port.direction == "inout":
            drive_storage = sv_inout_storage_name(port, "drive")
            enable_storage = sv_inout_storage_name(port, "oe")
            for indices in sv_index_tuples(port):
                suffix = "".join(f"[{item}]" for item in indices)
                lines.extend(
                    [
                        f"    {drive_storage}{suffix} = '0;",
                        f"    {enable_storage}{suffix} = 1'b0;",
                    ]
                )
    lines.extend(
        [
            "    sim_cycles = 0;",
            "    timer_generation = 0;",
            "    timer_deadline = NO_TIMER;",
            "    timer_owner_target = NO_TIMER;",
            f"    iterations = {default_iterations};",
            "    status = 0;",
            "    track_falling_edges = 1'b0;",
            "    clock_drivers_active = 1'b0;",
            "`ifdef CPPTB_SV_DPI_TIMING",
            "    phase_requests = 0;",
            "    phase_barrier_token = 1'b0;",
            "    last_started_time = $time;",
            "`endif",
        ]
    )
    if iteration_plusarg is not None:
        if not isinstance(iteration_plusarg, str) or not iteration_plusarg:
            raise CodegenError(
                "run.iteration_plusarg must be a non-empty string or null"
            )
        lines.append(
            f'    void\'($value$plusargs("{iteration_plusarg}=%d", iterations));'
        )
    lines.extend(
        [
            "",
            "    for (int i = 0; i < INPUT_WORD_COUNT; i++) begin",
            "      in_words[i] = '0;",
            "    end",
            "    for (int i = 0; i < OUTPUT_WORD_COUNT; i++) begin",
            "      out_words[i] = '0;",
            "    end",
            "    for (int i = 0; i < CPPTB_SIGNAL_COUNT; i++) begin",
            "      edge_interest[i] = '0;",
            "      registered_clock[i] = 1'b0;",
            "      primary_clock[i] = 1'b0;",
            "    end",
            "",
            f"    {init_function}(iterations, TIMEPRECISION_FS);",
            "`ifdef CPPTB_ENABLE_SV_LOGGING",
            "    cpptb_log_pkg::configure();",
            "`endif",
        ]
    )
    if dynamic_clocks:
        for port in dynamic_clock_ports:
            constant = sv_signal_constant(port)
            lines.extend(
                [
                    f"    registered_clock[{constant}] =",
                    f"        {clock_config_function}({constant}, 0) != 0;",
                    f"    primary_clock[{constant}] =",
                    f"        {clock_config_function}({constant}, 2) != 0;",
                ]
            )
    lines.extend(
        [
            "`ifdef CPPTB_SV_DPI_CALENDAR_TIMING",
            "    calendar_initialize();",
            "`endif",
            "    run_step(PHASE_INIT, NO_SIGNAL, EDGE_RISING, initial_requests);",
            "    service_requests(initial_requests);",
            "    clock_drivers_active = 1'b1;",
        ]
    )
    lines.extend(
        [
            "    if (status == 0) begin",
            "      fork",
            "`ifdef CPPTB_SV_DPI_CALENDAR_TIMING",
            "        calendar_owner();",
            "`else",
            "        timer_owner();",
            "`ifdef CPPTB_SV_DPI_TIMING",
            "        phase_pump();",
            "`endif",
        ]
    )
    for index, clock in enumerate(clocks):
        if clock_source(clock) in {"generated", "registered"}:
            lines.append(f"        drive_clock_{index}();")
    if dynamic_clocks:
        for index, _ in enumerate(dynamic_clock_ports):
            lines.append(f"        drive_registered_clock_{index}();")
    lines.append("`endif")
    for index, clock in enumerate(clocks):
        if clock_source(clock) not in {"generated", "registered"}:
            lines.append(f"        observe_clock_{index}();")
    for index, _ in enumerate(edge_observers):
        lines.append(f"        observe_signal_{index}();")
    for index, _ in enumerate(hierarchy_edge_paths_selected):
        lines.append(f"        observe_hierarchy_signal_{index}();")
    lines.extend(["      join_none", "    end"])
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
    dut_connections: list[tuple[str, str]] = [
        (port.name, port.name)
        for port in ports
        if port.interface_name is None
    ]
    dut_connections.extend(
        (interface.name, interface.name) for interface in interfaces
    )
    for index, (port_name, signal_name) in enumerate(dut_connections):
        comma = "," if index + 1 < len(dut_connections) else ""
        lines.append(f"      .{port_name}({signal_name}){comma}")
    lines.extend(["  );", ""])
    lines.extend(sv_port_export_functions(ports, manifest))
    lines.extend(sv_inout_export_functions(ports, manifest))
    lines.extend(sv_internal_export_functions(internals, manifest))
    lines.extend(
        sv_hierarchy_export_functions(
            hierarchy,
            list(manifest.get("hierarchy_accesses", [])),
            manifest,
        )
    )
    lines.extend([f"endmodule : {manifest['top_module']}", ""])
    return "\n".join(lines)


def output_paths(manifest: dict[str, Any], base_dir: Path) -> dict[str, Path]:
    outputs = manifest.get("outputs", {})
    required = ("cpp_dut", "cpp_binding", "sv_wrapper", "cpp_include")
    for key in required:
        if key not in outputs:
            raise CodegenError(f"manifest outputs is missing required key {key!r}")
    paths = {
        key: (base_dir / outputs[key]).resolve()
        for key in ("cpp_dut", "cpp_binding", "sv_wrapper")
    }
    if "cpp_adapter" in outputs:
        if "cpp_binding_include" not in outputs:
            raise CodegenError(
                "manifest outputs with cpp_adapter must define "
                "cpp_binding_include"
            )
        paths["cpp_adapter"] = (base_dir / outputs["cpp_adapter"]).resolve()
    if "cpp_clock_discovery" in outputs:
        if "cpp_binding_include" not in outputs:
            raise CodegenError(
                "manifest outputs with cpp_clock_discovery must define "
                "cpp_binding_include"
            )
        paths["cpp_clock_discovery"] = (
            base_dir / outputs["cpp_clock_discovery"]
        ).resolve()
    if "cpp_public_include" in outputs:
        paths["cpp_public_include"] = (
            base_dir / outputs["cpp_public_include"]
        ).resolve()
    return paths


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
        transport = getattr(signature, "transport", "packed")
        return (
            f"  {direction} {name}[{width}]{unpacked} "
            f"signed={signed} four_state={four_state} transport={transport}"
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


def hierarchy_catalog_data(design: DesignIR) -> dict[str, Any]:
    def dimensions(value: Port | HierarchySignal) -> list[dict[str, int]]:
        return [
            {"left": dimension.left, "right": dimension.right}
            for dimension in value.unpacked
        ]

    def declared_type(value: Port | HierarchySignal) -> str | None:
        packed_type = value.packed_type
        return packed_type.declared_name if packed_type is not None else None

    return {
        "schema_version": 1,
        "module": design.module,
        "ports": [
            {
                "name": port.name,
                "direction": port.direction,
                "width": port.width,
                "signed": port.signed,
                "four_state": port.four_state,
                "type_kind": port.type_kind,
                "declared_type": declared_type(port),
                "unpacked": dimensions(port),
            }
            for port in design.ports
        ],
        "scopes": [
            {
                "path": scope.hdl_path,
                "cpp_path": list(scope.cpp_path),
                "kind": scope.symbol_kind,
            }
            for scope in design.hierarchy.scopes
        ],
        "signals": [
            {
                "path": signal.hdl_path,
                "cpp_path": list(signal.cpp_path),
                "kind": signal.symbol_kind,
                "width": signal.width,
                "signed": signal.signed,
                "four_state": signal.four_state,
                "type_kind": signal.type_kind,
                "declared_type": declared_type(signal),
                "unpacked": dimensions(signal),
                "depositable": signal.depositable,
                "forceable": True,
            }
            for signal in design.hierarchy.signals
        ],
        "parameters": [
            {
                "path": parameter.hdl_path,
                "cpp_path": list(parameter.cpp_path),
                "value": parameter.value,
                "local": parameter.local,
            }
            for parameter in design.hierarchy.parameters
        ],
    }


def render_hierarchy_inspection(design: DesignIR) -> str:
    lines = [f"DUT {design.module}", "", "Ports"]
    for port in design.ports:
        state = "logic" if port.four_state else "bit"
        dimensions = "".join(
            f" [{dimension.left}:{dimension.right}]"
            for dimension in port.unpacked
        )
        lines.append(
            f"  {port.direction:6} {port.name}: {state}[{port.width}]"
            f"{dimensions}"
        )

    lines.extend(["", "Hierarchy"])
    for signal in design.hierarchy.signals:
        state = "logic" if signal.four_state else "bit"
        dimensions = "".join(
            f" [{dimension.left}:{dimension.right}]"
            for dimension in signal.unpacked
        )
        access = "get/deposit/force/release" if signal.depositable else (
            "get/force/release"
        )
        lines.append(
            f"  {signal.hdl_path}: {signal.symbol_kind} "
            f"{state}[{signal.width}]{dimensions} ({access})"
        )
    if not design.hierarchy.signals:
        lines.append("  <none>")

    lines.extend(["", "Parameters"])
    for parameter in design.hierarchy.parameters:
        lines.append(f"  {parameter.hdl_path} = {parameter.value}")
    if not design.hierarchy.parameters:
        lines.append("  <none>")
    return "\n".join(lines) + "\n"


def elaborate_inspection(
    inputs: list[Path],
    *,
    top: str | None,
    frontend: str | None,
    base_dir: Path,
) -> DesignIR:
    legacy_manifest = len(inputs) == 1 and inputs[0].suffix.lower() == ".json"
    if legacy_manifest:
        manifest_path = (base_dir / inputs[0]).resolve()
        manifest = load_manifest(manifest_path)
        return apply_port_transport(
            elaborate_design(manifest, manifest_path.parent, frontend),
            manifest,
        )

    if any(path.suffix.lower() == ".json" for path in inputs):
        raise CodegenError("a v1 manifest must be the only positional input")
    resolved_sources = [
        (path if path.is_absolute() else base_dir / path).resolve()
        for path in inputs
    ]
    inference_manifest = {
        "sources": [str(source) for source in resolved_sources],
        "frontend_options": {"slang": {"standard": "1800-2023"}},
    }
    if top is None:
        from cpptb_codegen.frontends.slang import infer_top_module

        top = infer_top_module(inference_manifest, base_dir)
    manifest = source_manifest(
        resolved_sources,
        top,
        output_dir=base_dir / "generated",
        frontend=frontend or "slang",
    )
    return apply_port_transport(
        elaborate_design(manifest, base_dir, frontend), manifest
    )


def generate_manifest(
    manifest: dict[str, Any],
    base_dir: Path,
    source: str,
    check: bool = False,
    frontend: str | None = None,
    compare_frontend: str | None = None,
) -> list[Path]:
    primary_name = frontend_name(manifest, frontend)
    design = apply_port_transport(
        elaborate_design(manifest, base_dir, frontend), manifest
    )
    if compare_frontend is not None:
        comparison_manifest = manifest
        if compare_frontend == "verilator_json" and manifest.get("internals"):
            comparison_manifest = {**manifest, "internals": []}
        comparison = apply_port_transport(
            elaborate_design(comparison_manifest, base_dir, compare_frontend),
            manifest,
        )
        compare_designs(design, comparison, primary_name, compare_frontend)

    discovered_ports = list(design.ports)
    internals = list(design.internals)
    hierarchy_accesses = list(manifest.get("hierarchy_accesses", []))
    validate_transport_ports(discovered_ports)
    validate_internals(internals)
    validate_hierarchy_accesses(design.hierarchy, hierarchy_accesses)
    ports = map_ports(discovered_ports, manifest)
    root = build_tree([*ports, *internals])

    validate_clock_ports(manifest, ports)

    paths = output_paths(manifest, base_dir)
    generated = {
        paths["cpp_dut"]: render_cpp_dut(
            ports,
            internals,
            root,
            manifest,
            source,
            design.hierarchy,
            design.interfaces,
        ),
        paths["cpp_binding"]: render_cpp_binding(
            ports, internals, root, manifest, source
        ),
        paths["sv_wrapper"]: render_sv(
            ports,
            internals,
            manifest,
            source,
            design.hierarchy,
            design.interfaces,
        ),
    }
    if "cpp_adapter" in paths:
        generated[paths["cpp_adapter"]] = render_cpp_adapter(manifest, source)
    if "cpp_clock_discovery" in paths:
        generated[paths["cpp_clock_discovery"]] = render_cpp_clock_discovery(
            manifest, source
        )
    if "cpp_public_include" in paths:
        generated[paths["cpp_public_include"]] = render_cpp_public_include(
            manifest, source
        )
    write_or_check(generated, check)
    return list(generated)


def generate(
    manifest_path: Path,
    check: bool = False,
    frontend: str | None = None,
    compare_frontend: str | None = None,
    clock_config: Path | None = None,
    access_config: Path | None = None,
) -> list[Path]:
    manifest_path = manifest_path.resolve()
    manifest = load_manifest(manifest_path)
    if clock_config is not None:
        resolved_clock_config = (
            clock_config
            if clock_config.is_absolute()
            else Path.cwd() / clock_config
        ).resolve()
        manifest["clocks"] = load_discovered_clocks(resolved_clock_config)
        manifest.pop("clock", None)
        manifest["run"] = {**manifest.get("run", {}), "dynamic_clocks": False}
    if access_config is not None:
        resolved_access_config = (
            access_config
            if access_config.is_absolute()
            else Path.cwd() / access_config
        ).resolve()
        hierarchy_accesses, port_edges = load_access_plan(
            resolved_access_config
        )
        manifest["hierarchy_accesses"] = hierarchy_accesses
        manifest["edge_observer_signal_ids"] = port_edges
    return generate_manifest(
        manifest,
        manifest_path.parent,
        manifest_path.name,
        check=check,
        frontend=frontend,
        compare_frontend=compare_frontend,
    )


def generate_sources(
    sources: list[Path],
    *,
    top: str | None = None,
    clocks: list[str] | None = None,
    primary_clock: str | None = None,
    edge_observers: list[str] | None = None,
    output_dir: Path | None = None,
    target: str | None = None,
    namespace: str | None = None,
    root_type: str = "Dut",
    check: bool = False,
    frontend: str = "slang",
    compare_frontend: str | None = None,
    clock_config: Path | None = None,
    access_config: Path | None = None,
    base_dir: Path | None = None,
    include_dirs: list[Path] | None = None,
    defines: list[str] | None = None,
    parameters: dict[str, str | int] | None = None,
) -> list[Path]:
    base_dir = (base_dir or Path.cwd()).resolve()
    resolved_sources = [
        (source if source.is_absolute() else base_dir / source).resolve()
        for source in sources
    ]
    resolved_include_dirs = [
        (path if path.is_absolute() else base_dir / path).resolve()
        for path in include_dirs or []
    ]
    if not resolved_sources:
        raise CodegenError("source-driven generation requires at least one source")

    inference_manifest = {
        "sources": [str(source) for source in resolved_sources],
        "include_dirs": [str(path) for path in resolved_include_dirs],
        "defines": defines or [],
        "parameters": parameters or {},
        "frontend_options": {
            "slang": {"standard": "1800-2023"},
        },
    }
    if top is None:
        from cpptb_codegen.frontends.slang import infer_top_module

        top = infer_top_module(inference_manifest, base_dir)

    if output_dir is None:
        output_dir = base_dir / "build" / "cpptb" / (target or top) / "generated"
    elif not output_dir.is_absolute():
        output_dir = base_dir / output_dir

    clock_arguments = clocks or []
    if clock_config is not None and clock_arguments:
        raise CodegenError("--clock-config cannot be combined with --clock")
    if clock_config is not None and primary_clock is not None:
        raise CodegenError(
            "--primary-clock cannot be combined with --clock-config"
        )
    if len(clock_arguments) > 1 and primary_clock is None:
        raise CodegenError(
            "multiple generated clocks require --primary-clock"
        )
    if clock_config is not None:
        resolved_clock_config = (
            clock_config
            if clock_config.is_absolute()
            else base_dir / clock_config
        ).resolve()
        parsed_clocks = load_discovered_clocks(resolved_clock_config)
    else:
        parsed_clocks = []
        for value in clock_arguments:
            port = value.split("=", 1)[0]
            parsed_clocks.append(
                parse_clock_argument(
                    value,
                    primary=(len(clock_arguments) == 1 or port == primary_clock),
                )
            )
    if primary_clock is not None and not any(
        clock["port"] == primary_clock for clock in parsed_clocks
    ):
        raise CodegenError(
            f"primary clock {primary_clock!r} is not configured by --clock"
        )

    manifest = source_manifest(
        resolved_sources,
        top,
        output_dir=output_dir,
        clocks=parsed_clocks,
        edge_observers=edge_observers,
        target=target,
        namespace=namespace,
        root_type=root_type,
        frontend=frontend,
        dynamic_clocks=clock_config is None,
        include_dirs=resolved_include_dirs,
        defines=defines,
        parameters=parameters,
    )
    if access_config is not None:
        resolved_access_config = (
            access_config
            if access_config.is_absolute()
            else base_dir / access_config
        ).resolve()
        hierarchy_accesses, port_edges = load_access_plan(
            resolved_access_config
        )
        manifest["hierarchy_accesses"] = hierarchy_accesses
        manifest["edge_observer_signal_ids"] = port_edges
    return generate_manifest(
        manifest,
        base_dir,
        ", ".join(source.name for source in resolved_sources),
        check=check,
        compare_frontend=compare_frontend,
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "inputs",
        nargs="+",
        type=Path,
        help="a v1 manifest or one or more SystemVerilog sources",
    )
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
    parser.add_argument(
        "--top", help="select the DUT top module when source inference is ambiguous"
    )
    parser.add_argument(
        "--clock",
        action="append",
        default=[],
        metavar="PORT=PERIOD[@PHASE]",
        help="generate an explicitly timed clock; may be repeated",
    )
    parser.add_argument(
        "--primary-clock",
        help="select the primary clock when more than one --clock is present",
    )
    parser.add_argument(
        "--clock-config",
        type=Path,
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--access-config",
        type=Path,
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--edge-observer",
        action="append",
        default=[],
        help="observe edges on this DUT output; may be repeated",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        help=(
            "generated artifact directory "
            "(default: build/cpptb/TARGET/generated)"
        ),
    )
    parser.add_argument("--target", help="override the generated target identifier")
    parser.add_argument(
        "--namespace", help="override the target-unique generated C++ namespace"
    )
    parser.add_argument(
        "--root-type",
        default="Dut",
        help="override the generated typed root name (default: Dut)",
    )
    parser.add_argument(
        "--inspect-hierarchy",
        action="store_true",
        help="print the complete elaborated DUT hierarchy and exit",
    )
    parser.add_argument(
        "--hierarchy-json",
        type=Path,
        help="write the inferred hierarchy as an optional JSON snapshot",
    )
    parser.add_argument(
        "--check-hierarchy",
        type=Path,
        help="fail if an existing hierarchy JSON snapshot is stale",
    )
    args = parser.parse_args(argv)
    try:
        if args.hierarchy_json is not None and args.check_hierarchy is not None:
            raise CodegenError(
                "--hierarchy-json and --check-hierarchy are mutually exclusive"
            )
        if (args.inspect_hierarchy or args.hierarchy_json is not None or
                args.check_hierarchy is not None):
            design = elaborate_inspection(
                args.inputs,
                top=args.top,
                frontend=args.frontend,
                base_dir=Path.cwd(),
            )
            if args.inspect_hierarchy:
                print(render_hierarchy_inspection(design), end="")
            snapshot = json.dumps(
                hierarchy_catalog_data(design), indent=2, sort_keys=True
            ) + "\n"
            if args.hierarchy_json is not None:
                args.hierarchy_json.parent.mkdir(parents=True, exist_ok=True)
                args.hierarchy_json.write_text(snapshot)
                print(f"wrote hierarchy {args.hierarchy_json}")
            if args.check_hierarchy is not None:
                try:
                    current = args.check_hierarchy.read_text()
                except OSError as error:
                    raise CodegenError(
                        f"cannot read hierarchy snapshot "
                        f"{args.check_hierarchy}: {error}"
                    ) from error
                if current != snapshot:
                    raise CodegenError(
                        f"hierarchy snapshot is stale: "
                        f"{args.check_hierarchy}"
                    )
                print(f"checked hierarchy {args.check_hierarchy}")
            return 0
        legacy_manifest = (
            len(args.inputs) == 1 and args.inputs[0].suffix.lower() == ".json"
        )
        if legacy_manifest:
            source_only_options = (
                args.top,
                args.clock or None,
                args.primary_clock,
                args.edge_observer or None,
                args.output_dir,
                args.target,
                args.namespace,
            )
            if any(value is not None for value in source_only_options) or (
                args.root_type != "Dut"
            ):
                raise CodegenError(
                    "--top, --clock, --output-dir, --target, --namespace, and "
                    "source metadata options are only valid with source inputs"
                )
            paths = generate(
                args.inputs[0],
                check=args.check,
                frontend=args.frontend,
                compare_frontend=args.compare_frontend,
                clock_config=args.clock_config,
                access_config=args.access_config,
            )
        else:
            if any(path.suffix.lower() == ".json" for path in args.inputs):
                raise CodegenError(
                    "a v1 manifest must be the only positional input"
                )
            paths = generate_sources(
                args.inputs,
                top=args.top,
                clocks=args.clock,
                primary_clock=args.primary_clock,
                edge_observers=args.edge_observer,
                output_dir=args.output_dir,
                target=args.target,
                namespace=args.namespace,
                root_type=args.root_type,
                check=args.check,
                frontend=args.frontend or "slang",
                compare_frontend=args.compare_frontend,
                clock_config=args.clock_config,
                access_config=args.access_config,
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
