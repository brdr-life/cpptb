"""Slang frontend using pyslang's typed elaborated AST."""

from __future__ import annotations

import shlex
from pathlib import Path
from typing import Any

from cpptb.codegen.design_ir import (
    CodegenError,
    DesignIR,
    Internal,
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


def _port_shape(port_type: Any) -> tuple[Any, tuple[UnpackedRange, ...]]:
    dimensions: list[UnpackedRange] = []
    element_type = port_type
    while element_type.isUnpackedArray:
        if not element_type.isFixedSize:
            break
        declared_range = element_type.range
        dimensions.append(
            UnpackedRange(int(declared_range.left), int(declared_range.right))
        )
        element_type = element_type.elementType
    return element_type, tuple(dimensions)


def _declared_type_name(data_type: Any) -> str | None:
    if not data_type.isAlias:
        return None
    name = str(data_type.name)
    return name or None


def _packed_ranges(data_type: Any) -> tuple[PackedRange, ...]:
    ranges: list[PackedRange] = []
    element_type = data_type.canonicalType
    while element_type.isPackedArray:
        declared_range = element_type.range
        ranges.append(
            PackedRange(int(declared_range.left), int(declared_range.right))
        )
        element_type = element_type.elementType.canonicalType
    return tuple(ranges)


def _packed_type(data_type: Any) -> PackedType:
    canonical = data_type.canonicalType
    declared_name = _declared_type_name(data_type)

    if canonical.isEnum:
        base = _packed_type(canonical.baseType)
        if not isinstance(base, PackedIntegralType):
            raise CodegenError("packed enum base type is not a plain integral type")
        values: list[PackedEnumValue] = []
        for value in canonical:
            integer = value.value.value
            if integer.hasUnknown:
                raise CodegenError(
                    f"packed enum value {value.name!r} contains X or Z bits"
                )
            values.append(PackedEnumValue(value.name, int(integer)))
        return PackedEnumType(base, tuple(values), declared_name)

    if canonical.isStruct and not canonical.isUnpackedStruct:
        fields = tuple(
            PackedField(
                member.name,
                _packed_type(member.type),
                int(member.bitOffset),
            )
            for member in canonical
        )
        return PackedStructType(
            int(canonical.bitWidth),
            bool(canonical.isSigned),
            bool(canonical.isFourState),
            fields,
            declared_name,
        )

    if canonical.isPackedUnion:
        fields = tuple(
            PackedField(
                member.name,
                _packed_type(member.type),
                int(member.bitOffset),
            )
            for member in canonical
        )
        return PackedUnionType(
            int(canonical.bitWidth),
            bool(canonical.isSigned),
            bool(canonical.isFourState),
            fields,
            declared_name,
        )

    if not canonical.isIntegral:
        raise CodegenError(
            f"cannot describe non-integral packed type {canonical.kind}"
        )
    return PackedIntegralType(
        int(canonical.bitWidth),
        bool(canonical.isSigned),
        bool(canonical.isFourState),
        _packed_ranges(canonical),
        declared_name,
    )


def _transport_kind(data_type: PackedType) -> str:
    return "packed_union" if isinstance(data_type, PackedUnionType) else "integral"


def _internal_cpp_path(config: dict[str, Any], hdl_path: str) -> tuple[str, ...]:
    configured = config.get("name", hdl_path)
    if not isinstance(configured, str) or not configured:
        raise CodegenError(f"internal {hdl_path!r} name must be a non-empty string")
    return ("internal", *configured.split("."))


def _resolve_internals(
    pyslang: Any, top: Any, manifest: dict[str, Any]
) -> tuple[Internal, ...]:
    configs = manifest.get("internals", [])
    if not isinstance(configs, list):
        raise CodegenError("manifest internals must be a list")

    internals: list[Internal] = []
    seen_paths: set[str] = set()
    for config in configs:
        if not isinstance(config, dict):
            raise CodegenError("each internal configuration must be an object")
        hdl_path = config.get("path")
        if not isinstance(hdl_path, str) or not hdl_path:
            raise CodegenError("each internal must define a non-empty path")
        if hdl_path in seen_paths:
            raise CodegenError(
                f"internal path {hdl_path!r} is configured more than once"
            )
        seen_paths.add(hdl_path)

        access = config.get("access", "read")
        if access not in {"read", "read_write"}:
            raise CodegenError(
                f"internal {hdl_path!r} has unsupported access {access!r}; "
                "expected 'read' or 'read_write'"
            )
        forceable = config.get("force", False)
        if not isinstance(forceable, bool):
            raise CodegenError(
                f"internal {hdl_path!r} force must be a boolean"
            )

        symbol = top.body.lookupName(hdl_path)
        if symbol is None:
            raise CodegenError(f"internal path {hdl_path!r} was not found")
        if symbol.kind == pyslang.ast.SymbolKind.Variable:
            symbol_kind = "variable"
        elif symbol.kind == pyslang.ast.SymbolKind.Net:
            symbol_kind = "net"
        else:
            raise CodegenError(
                f"internal path {hdl_path!r} resolves to unsupported symbol "
                f"kind {symbol.kind}"
            )
        if access == "read_write" and symbol_kind != "variable":
            raise CodegenError(
                f"internal path {hdl_path!r} is a {symbol_kind}; "
                "read_write access requires a variable"
            )

        element_type, unpacked = _port_shape(symbol.type)
        if forceable and unpacked and symbol_kind != "variable":
            raise CodegenError(
                f"internal path {hdl_path!r} is a {symbol_kind}; "
                "forceable memories require a variable"
            )
        packed_type = (
            _packed_type(element_type) if element_type.isIntegral else None
        )
        kind = _transport_kind(packed_type) if packed_type else _port_kind(element_type)
        width = int(element_type.bitWidth) if element_type.isIntegral else 0
        internals.append(
            Internal(
                hdl_path=hdl_path,
                cpp_path=_internal_cpp_path(config, hdl_path),
                symbol_kind=symbol_kind,
                width=width,
                access=access,
                forceable=forceable,
                type_kind=kind,
                signed=bool(element_type.isSigned),
                four_state=bool(element_type.isFourState),
                unpacked=unpacked,
                packed_type=packed_type,
            )
        )
    return tuple(internals)


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
            port_type, unpacked = _port_shape(symbol.type)
            packed_type = (
                _packed_type(port_type) if port_type.isIntegral else None
            )
            kind = (
                _transport_kind(packed_type)
                if packed_type
                else _port_kind(port_type)
            )
            width = int(port_type.bitWidth) if port_type.isIntegral else 0
            ports.append(
                Port(
                    name=symbol.name,
                    direction=direction,
                    width=width,
                    type_kind=kind,
                    signed=bool(port_type.isSigned),
                    four_state=bool(port_type.isFourState),
                    unpacked=unpacked,
                    packed_type=packed_type,
                )
            )

        if not ports:
            raise CodegenError(
                f"module {manifest['module']!r} has no discoverable ports"
            )
        return DesignIR(
            manifest["module"], tuple(ports), _resolve_internals(pyslang, top, manifest)
        )
