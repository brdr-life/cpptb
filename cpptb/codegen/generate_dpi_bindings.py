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

from cpptb.codegen.design_ir import CodegenError, DesignIR, Port
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
    children: OrderedDict[str, TreeNode | Port] = field(default_factory=OrderedDict)


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


def validate_transport_ports(ports: list[Port]) -> None:
    for port in ports:
        if port.type_kind != "integral" or port.width < 1:
            raise CodegenError(
                f"port {port.name!r} uses unsupported elaborated type "
                f"{port.type_kind!r}; only scalar and packed integral ports "
                "are supported"
            )
        if port.direction not in {"input", "output"}:
            raise CodegenError(
                f"port {port.name!r} has direction {port.direction!r}; "
                "the current DPI transport only supports input and output ports"
            )
        if port.width > 32:
            raise CodegenError(
                f"port {port.name!r} is {port.width} bits wide; the current uint32 "
                "DPI transport supports at most 32 bits"
            )


def build_tree(ports: list[Port]) -> TreeNode:
    root = TreeNode(())
    for port in ports:
        node = root
        for component in port.cpp_path[:-1]:
            existing = node.children.get(component)
            if isinstance(existing, Port):
                raise CodegenError(
                    f"C++ path {'.'.join(port.cpp_path)!r} traverses a signal"
                )
            if existing is None:
                existing = TreeNode(node.path + (component,))
                node.children[component] = existing
            node = existing
        leaf_name = port.cpp_path[-1]
        if leaf_name in node.children:
            raise CodegenError(f"duplicate C++ member path {'.'.join(port.cpp_path)!r}")
        node.children[leaf_name] = port
    return root


def pascal_case(value: str) -> str:
    parts = re.findall(r"[A-Za-z]+|\d+", value.replace("_", " "))
    return "".join(part[:1].upper() + part[1:].lower() for part in parts)


def signal_id(port_name: str) -> str:
    return "kSignal" + pascal_case(port_name)


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
    for name, child in node.children.items():
        if isinstance(child, Port):
            signature.append((name, "signal"))
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


def render_cpp_dut(
    ports: list[Port], root: TreeNode, manifest: dict[str, Any], source: str
) -> str:
    lines = [
        generated_banner(source).rstrip(),
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        '#include "cpptb/coro_runtime.hpp"',
        "",
        f"namespace {manifest['namespace']} {{",
        "",
        "enum SignalId : uint32_t {",
    ]
    lines.extend(f"    {signal_id(port.name)}," for port in ports)
    lines.extend(["    kSignalCount,", "};", ""])

    for node in collect_structs(root, manifest):
        lines.append(f"struct {node_type(node, manifest)} {{")
        for name, child in node.children.items():
            field_type = (
                "coro::Signal" if isinstance(child, Port) else node_type(child, manifest)
            )
            lines.append(f"    {field_type} {name};")
        lines.extend(["};", ""])

    lines.append(f"}}  // namespace {manifest['namespace']}")
    lines.append("")
    return "\n".join(lines)


def render_binding_expr(node: TreeNode, indent: int = 4) -> list[str]:
    prefix = " " * indent
    lines = [prefix + "{"]
    values = list(node.children.values())
    for index, child in enumerate(values):
        comma = "," if index + 1 < len(values) else ""
        if isinstance(child, Port):
            lines.append(
                " " * (indent + 4)
                + f'make_signal({signal_id(child.name)}, "{child.name}"){comma}'
            )
        else:
            child_lines = render_binding_expr(child, indent + 4)
            child_lines[-1] += comma
            lines.extend(child_lines)
    lines.append(prefix + "}")
    return lines


def render_cpp_binding(
    ports: list[Port], root: TreeNode, manifest: dict[str, Any], source: str
) -> str:
    clock_names = {clock["port"] for clock in clock_configs(manifest)}
    generated_clocks = generated_clock_names(manifest)
    driven = [
        port
        for port in ports
        if port.direction == "input" and port.name not in generated_clocks
    ]
    include = manifest["outputs"]["cpp_include"]
    lines = [
        generated_banner(source).rstrip(),
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstdint>",
        "#include <utility>",
        "",
        f'#include "{include}"',
        "",
        f"namespace {manifest['namespace']}::generated {{",
        "",
        f"inline constexpr std::array<uint32_t, {len(clock_names)}> kClockSignalIds = {{",
    ]
    lines.extend(
        f"    {signal_id(clock['port'])}," for clock in clock_configs(manifest)
    )
    lines.extend([
        "};",
        f"inline constexpr std::array<uint32_t, {len(driven)}> kDrivenSignalIds = {{",
    ])
    lines.extend(f"    {signal_id(port.name)}," for port in driven)
    lines.extend(["};", "", "template <typename MakeSignal>"])
    lines.append(
        f"{manifest['root_type']} bind_dut(MakeSignal&& make_signal) {{"
    )
    expression = render_binding_expr(root, 4)
    expression[0] = "    return " + expression[0].lstrip()
    expression[-1] += ";"
    lines.extend(expression)
    lines.extend(["}", "", f"}}  // namespace {manifest['namespace']}::generated", ""])
    return "\n".join(lines)


def sv_decl(port: Port) -> str:
    packed = "" if port.width == 1 else f" [{port.width - 1}:0]"
    return f"  logic{packed} {port.name};"


def sv_output_assignment(port: Port) -> str:
    source = f"out_words[{signal_id(port.name).replace('kSignal', 'SIGNAL_').upper()}]"
    if port.width == 1:
        source += "[0]"
    elif port.width < 32:
        source += f"[{port.width - 1}:0]"
    return f"    {port.name} = {source};"


def sv_signal_constant(port: Port) -> str:
    name = signal_id(port.name).replace("kSignal", "SIGNAL_").upper()
    return name


def render_sv(ports: list[Port], manifest: dict[str, Any], source: str) -> str:
    clocks = clock_configs(manifest)
    generated_clocks = generated_clock_names(manifest)
    primary_clocks = [clock for clock in clocks if clock.get("primary", False)]
    if len(primary_clocks) > 1:
        raise CodegenError("only one clock may be marked primary")
    primary_clock = primary_clocks[0] if primary_clocks else (clocks[0] if clocks else None)
    run = manifest.get("run", {})
    init_function = run.get("init_function", "cpptb_dpi_init")
    step_function = run.get("step_function", "cpptb_dpi_step")
    next_deadline_function = run.get(
        "next_deadline_function", "cpptb_dpi_next_timer_deadline"
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
            "  localparam int unsigned NO_SIGNAL = 32'hffff_ffff;",
            "  localparam longint unsigned NO_TIMER = 64'hffff_ffff_ffff_ffff;",
            "",
            "  localparam int STEP_DONE = 1;",
            "  localparam int STEP_TIMER_CHANGED = 8;",
            "  localparam int STEP_FALLING_EDGES = 16;",
            "  localparam int STEP_OUTPUTS_CHANGED = 32;",
            "",
        ]
    )
    for index, port in enumerate(ports):
        lines.append(f"  localparam int {sv_signal_constant(port)} = {index};")
    lines.extend(
        [
            f"  localparam int SIGNAL_COUNT = {len(ports)};",
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
            "      input int unsigned in_words[],",
            "      output int unsigned out_words[]",
            "  );",
            f'  import "DPI-C" context function longint unsigned {next_deadline_function}();',
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
            "  int status;",
            "  bit track_falling_edges;",
            "  int initial_requests;",
            "  int unsigned in_words[0:SIGNAL_COUNT-1];",
            "  int unsigned out_words[0:SIGNAL_COUNT-1];",
            "",
            "  task automatic pack_inputs();",
        ]
    )
    lines.extend(
        f"    in_words[{sv_signal_constant(port)}] = {port.name};" for port in ports
    )
    lines.extend(["  endtask", "", "  task automatic apply_outputs();"])
    lines.extend(
        sv_output_assignment(port)
        for port in ports
        if port.direction == "input" and port.name not in generated_clocks
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
            "                               in_words, out_words);",
            "    apply_outputs();",
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
            "  task automatic reschedule_timer();",
            "    longint unsigned deadline;",
            "    longint unsigned generation;",
            f"    deadline = {next_deadline_function}();",
            "    timer_generation++;",
            "    generation = timer_generation;",
            "    if (deadline != NO_TIMER) begin",
            "      fork",
            "        timer_wakeup(deadline, generation);",
            "      join_none",
            "    end",
            "  endtask",
            "",
            "  task automatic service_requests(",
            "      input int initial_requests",
            "  );",
            "    int requests;",
            "    requests = initial_requests;",
            "    if ((requests & STEP_TIMER_CHANGED) != 0) begin",
            "      reschedule_timer();",
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

    lines.append("  initial begin")
    for clock in clocks:
        if clock_source(clock) != "generated":
            continue
        lines.append(f"    {clock['port']} = 1'b0;")
    for port in ports:
        if port.direction == "input" and port.name not in generated_clocks:
            lines.append(f"    {port.name} = '0;")
    lines.extend(
        [
            "    sim_cycles = 0;",
            "    timer_generation = 0;",
            f"    iterations = {default_iterations};",
            "    status = 0;",
            "    track_falling_edges = 1'b0;",
            f'    void\'($value$plusargs("{iteration_plusarg}=%d", iterations));',
            "",
            "    for (int i = 0; i < SIGNAL_COUNT; i++) begin",
            "      in_words[i] = '0;",
            "      out_words[i] = '0;",
            "    end",
            "",
            f"    {init_function}(iterations, TIMEPRECISION_FS);",
            "    run_step(PHASE_INIT, NO_SIGNAL, EDGE_RISING, initial_requests);",
            "    service_requests(initial_requests);",
        ]
    )
    if clocks:
        lines.extend(
            [
                "    if (status == 0) begin",
                "      fork",
            ]
        )
        for index, clock in enumerate(clocks):
            if clock_source(clock) == "generated":
                lines.append(f"        drive_clock_{index}();")
            else:
                lines.append(f"        observe_clock_{index}();")
        lines.extend(
            [
                "      join_none",
                "    end",
            ]
        )
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
    lines.extend(["  );", f"endmodule : {manifest['top_module']}", ""])
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

    primary_ports = "\n".join(
        f"  {direction} {name}[{width}]"
        for name, direction, width in primary.transport_signature()
    )
    comparison_ports = "\n".join(
        f"  {direction} {name}[{width}]"
        for name, direction, width in comparison.transport_signature()
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
        comparison = elaborate_design(manifest, base_dir, compare_frontend)
        compare_designs(design, comparison, primary_name, compare_frontend)

    discovered_ports = list(design.ports)
    validate_transport_ports(discovered_ports)
    ports = map_ports(discovered_ports, manifest)
    root = build_tree(ports)

    validate_clock_ports(manifest, ports)

    paths = output_paths(manifest, base_dir)
    source = manifest_path.name
    generated = {
        paths["cpp_dut"]: render_cpp_dut(ports, root, manifest, source),
        paths["cpp_binding"]: render_cpp_binding(ports, root, manifest, source),
        paths["sv_wrapper"]: render_sv(ports, manifest, source),
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
