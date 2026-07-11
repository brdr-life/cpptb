"""Slang frontend using pyslang's typed elaborated AST."""

from __future__ import annotations

import shlex
from pathlib import Path
from typing import Any

from cpptb.codegen.design_ir import CodegenError, DesignIR, Port
from cpptb.codegen.frontends import frontend_options


def _frontend_config(manifest: dict[str, Any]) -> dict[str, Any]:
    return frontend_options(manifest, "slang")


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


def _command_line(manifest: dict[str, Any], base_dir: Path) -> list[str]:
    config = _frontend_config(manifest)
    standard = config.get("standard", "1800-2023")
    args = [
        f"--std={standard}",
        f"--top={manifest['module']}",
        *(
            f"-I{(base_dir / include_dir).resolve()}"
            for include_dir in manifest.get("include_dirs", [])
        ),
        *_define_args(manifest),
        *(
            f"-G{name}={value}"
            for name, value in manifest.get("parameters", {}).items()
        ),
        *config.get("args", []),
        *(str((base_dir / source).resolve()) for source in manifest["sources"]),
    ]
    return args


def _diagnostic_text(pyslang: Any, compilation: Any) -> str:
    diagnostics = compilation.getAllDiagnostics()
    if not any(diagnostic.isError() for diagnostic in diagnostics):
        return ""
    return pyslang.DiagnosticEngine.reportAll(
        compilation.sourceManager, diagnostics
    ).rstrip()


def _port_kind(port_type: Any) -> str:
    if port_type.isIntegral:
        return "integral"
    if port_type.isUnpackedArray:
        return "unpacked_array"
    if port_type.isArray:
        return "array"
    return "unsupported"


class SlangFrontend:
    name = "slang"

    def elaborate(self, manifest: dict[str, Any], base_dir: Path) -> DesignIR:
        try:
            import pyslang
        except ImportError as error:
            raise CodegenError(
                "the Slang frontend requires pyslang; run code generation via "
                "`uv run python` or install the locked project dependencies"
            ) from error

        driver = pyslang.driver.Driver()
        driver.addStandardArgs()
        command_line = shlex.join(_command_line(manifest, base_dir))
        if not driver.parseCommandLine(command_line) or not driver.processOptions():
            raise CodegenError("Slang rejected the configured frontend options")

        driver.parseAllSources()
        compilation = driver.createCompilation()
        diagnostics = _diagnostic_text(pyslang, compilation)
        if diagnostics:
            raise CodegenError("Slang could not elaborate the DUT:\n" + diagnostics)

        top = next(
            (
                instance
                for instance in compilation.getRoot().topInstances
                if instance.name == manifest["module"]
            ),
            None,
        )
        if top is None:
            raise CodegenError(
                f"module {manifest['module']!r} was not found in Slang's "
                "elaborated top instances"
            )

        directions = {
            pyslang.ast.ArgumentDirection.In: "input",
            pyslang.ast.ArgumentDirection.Out: "output",
            pyslang.ast.ArgumentDirection.InOut: "inout",
            pyslang.ast.ArgumentDirection.Ref: "ref",
        }
        ports: list[Port] = []
        for symbol in top.body.portList:
            if symbol.kind == pyslang.ast.SymbolKind.InterfacePort:
                ports.append(
                    Port(
                        name=symbol.name,
                        direction="interface",
                        width=0,
                        type_kind="interface",
                    )
                )
                continue
            if symbol.kind != pyslang.ast.SymbolKind.Port:
                ports.append(
                    Port(
                        name=symbol.name,
                        direction="unsupported",
                        width=0,
                        type_kind=str(symbol.kind),
                    )
                )
                continue
            direction = directions.get(symbol.direction)
            if direction is None:
                raise CodegenError(
                    f"port {symbol.name!r} has unsupported Slang direction "
                    f"{symbol.direction}"
                )
            port_type = symbol.type
            kind = _port_kind(port_type)
            width = int(port_type.bitWidth) if port_type.isIntegral else 0
            ports.append(
                Port(
                    name=symbol.name,
                    direction=direction,
                    width=width,
                    type_kind=kind,
                    signed=bool(port_type.isSigned),
                    four_state=bool(port_type.isFourState),
                )
            )

        if not ports:
            raise CodegenError(f"module {manifest['module']!r} has no discoverable ports")
        return DesignIR(manifest["module"], tuple(ports))
