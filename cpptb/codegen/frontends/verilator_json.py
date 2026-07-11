"""Compatibility frontend for Verilator's JSON-only elaboration output."""

from __future__ import annotations

import json
import re
import subprocess
import tempfile
from pathlib import Path
from typing import Any

from cpptb.codegen.design_ir import CodegenError, DesignIR, Port
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
        ports.append(
            Port(
                name=name,
                direction=direction,
                width=parse_width(dtype, name),
                signed=bool(dtype.get("signed", False)),
                four_state=dtype.get("name") != "bit",
            )
        )

    if not ports:
        raise CodegenError(f"module {module_name!r} has no discoverable ports")
    return ports


class VerilatorJsonFrontend:
    name = "verilator_json"

    def elaborate(self, manifest: dict[str, Any], base_dir: Path) -> DesignIR:
        ast = verilator_ast(manifest, base_dir)
        ports = discover_ports(ast, manifest["module"])
        return DesignIR(manifest["module"], tuple(ports))
