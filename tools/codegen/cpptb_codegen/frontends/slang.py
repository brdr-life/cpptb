"""Slang frontend using pyslang's typed elaborated AST."""

from __future__ import annotations

import shlex
from pathlib import Path
from typing import Any

from cpptb_codegen.design_ir import (
    CodegenError,
    DesignIR,
    HierarchyCatalog,
    HierarchyParameter,
    HierarchyScope,
    HierarchySignal,
    Internal,
    InterfaceConstructorPort,
    InterfaceParameter,
    InterfacePort,
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
from cpptb_codegen.frontends import frontend_options


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


def _command_line(
    manifest: dict[str, Any], base_dir: Path, *, select_top: bool = True
) -> list[str]:
    config = _frontend_config(manifest)
    standard = config.get("standard", "1800-2023")
    args = [
        f"--std={standard}",
        *([f"--top={manifest['module']}"] if select_top else []),
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


def _create_compilation(
    manifest: dict[str, Any], base_dir: Path, *, select_top: bool = True
) -> tuple[Any, Any]:
    try:
        import pyslang
    except ImportError as error:
        raise CodegenError(
            "the Slang frontend requires pyslang; run code generation via "
            "`uv run python` or install the locked project dependencies"
        ) from error

    driver = pyslang.driver.Driver()
    driver.addStandardArgs()
    command_line = shlex.join(
        _command_line(manifest, base_dir, select_top=select_top)
    )
    if not driver.parseCommandLine(command_line) or not driver.processOptions():
        raise CodegenError("Slang rejected the configured frontend options")

    driver.parseAllSources()
    # A source-driven wrapper will provide concrete interface instances later.
    # Slang otherwise diagnoses interface ports on the selected design top as
    # unconnected before code generation has a chance to inspect them.
    compilation = driver.createCompilation()
    compilation.options.flags |= type(
        compilation.options.flags
    ).AllowTopLevelIfacePorts
    diagnostics = _diagnostic_text(pyslang, compilation)
    if diagnostics:
        raise CodegenError("Slang could not elaborate the DUT:\n" + diagnostics)
    return pyslang, compilation


def infer_top_module(manifest: dict[str, Any], base_dir: Path) -> str:
    _, compilation = _create_compilation(
        manifest, base_dir, select_top=False
    )
    candidates = sorted(
        instance.name for instance in compilation.getRoot().topInstances
    )
    if len(candidates) == 1:
        return candidates[0]
    if not candidates:
        raise CodegenError("cannot infer a top module: no top-level modules found")
    raise CodegenError(
        "cannot infer a top module: multiple top-level modules found: "
        + ", ".join(candidates)
        + "; select one with --top"
    )


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


def _declared_unpacked_ranges(ranges: Any) -> tuple[UnpackedRange, ...]:
    if ranges is None:
        return ()
    return tuple(
        UnpackedRange(int(item.left), int(item.right)) for item in ranges
    )


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


def _relative_hierarchy_path(top_name: str, hierarchical_path: str) -> str:
    prefix = f"{top_name}."
    if not hierarchical_path.startswith(prefix):
        raise CodegenError(
            f"Slang returned hierarchy path {hierarchical_path!r} outside "
            f"top instance {top_name!r}"
        )
    return hierarchical_path[len(prefix) :]


def _hierarchy_cpp_path(hdl_path: str) -> tuple[str, ...]:
    """Preserve array suffixes for grouping by the C++ hierarchy renderer."""

    return tuple(hdl_path.split("."))


def _elaborate_hierarchy(pyslang: Any, top: Any) -> HierarchyCatalog:
    scopes: dict[str, HierarchyScope] = {}
    signals: dict[str, HierarchySignal] = {}
    parameters: dict[str, HierarchyParameter] = {}
    top_port_paths = {
        f"{top.name}.{port.name}" for port in top.body.portList
    }
    scope_kinds = {
        pyslang.ast.SymbolKind.Instance: "instance",
        pyslang.ast.SymbolKind.InstanceArray: "instance_array",
        pyslang.ast.SymbolKind.GenerateBlock: "generate_block",
        pyslang.ast.SymbolKind.GenerateBlockArray: "generate_array",
    }

    def visit(symbol: Any) -> None:
        kind = getattr(symbol, "kind", None)
        hierarchical_path = getattr(symbol, "hierarchicalPath", "")
        if (
            not hierarchical_path
            or hierarchical_path == top.name
            or not hierarchical_path.startswith(f"{top.name}.")
        ):
            return

        hdl_path = _relative_hierarchy_path(top.name, hierarchical_path)
        cpp_path = _hierarchy_cpp_path(hdl_path)
        if kind in scope_kinds:
            scopes.setdefault(
                hdl_path,
                HierarchyScope(hdl_path, cpp_path, scope_kinds[kind]),
            )
            return

        if kind == pyslang.ast.SymbolKind.Parameter:
            try:
                converted = symbol.value.convertToInt()
                if converted.hasUnknown():
                    return
                value = int(converted.value)
            except Exception:
                return
            parameters.setdefault(
                hdl_path,
                HierarchyParameter(
                    hdl_path,
                    cpp_path,
                    value,
                    bool(symbol.isLocalParam),
                ),
            )
            return

        symbol_kinds = {
            pyslang.ast.SymbolKind.Variable: "variable",
            pyslang.ast.SymbolKind.Net: "net",
        }
        if kind not in symbol_kinds or hierarchical_path in top_port_paths:
            return
        try:
            element_type, unpacked = _port_shape(symbol.type)
        except CodegenError:
            return
        if not element_type.isIntegral:
            return
        packed_type = _packed_type(element_type)
        signals.setdefault(
            hdl_path,
            HierarchySignal(
                hdl_path=hdl_path,
                cpp_path=cpp_path,
                symbol_kind=symbol_kinds[kind],
                width=int(element_type.bitWidth),
                type_kind=_transport_kind(packed_type),
                signed=bool(element_type.isSigned),
                four_state=bool(element_type.isFourState),
                unpacked=unpacked,
                packed_type=packed_type,
            ),
        )

    top.body.visit(visit)
    return HierarchyCatalog(
        scopes=tuple(scopes[path] for path in sorted(scopes)),
        signals=tuple(signals[path] for path in sorted(signals)),
        parameters=tuple(parameters[path] for path in sorted(parameters)),
    )


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


def _first_interface_instance(pyslang: Any, connection: Any) -> Any:
    if connection.kind == pyslang.ast.SymbolKind.Instance:
        return connection
    if connection.kind != pyslang.ast.SymbolKind.InstanceArray:
        raise CodegenError(
            "Slang returned an unsupported interface connection kind "
            f"{connection.kind}"
        )
    pending = list(connection.elements)
    while pending:
        candidate = pending.pop(0)
        if candidate.kind == pyslang.ast.SymbolKind.Instance:
            return candidate
        if candidate.kind == pyslang.ast.SymbolKind.InstanceArray:
            pending[0:0] = list(candidate.elements)
    raise CodegenError("Slang returned an empty interface instance array")


def _resolved_parameter_text(symbol: Any, interface_name: str) -> str:
    try:
        converted = symbol.value.convertToInt()
        if converted.hasUnknown():
            raise CodegenError(
                f"interface {interface_name!r} parameter {symbol.name!r} "
                "contains X or Z bits"
            )
        return str(int(converted.value))
    except CodegenError:
        raise
    except Exception as error:
        raise CodegenError(
            f"interface {interface_name!r} parameter {symbol.name!r} is not "
            "an integral value parameter; type and non-integral interface "
            "parameters are not yet supported"
        ) from error


def _interface_contract(
    pyslang: Any, symbol: Any, directions: dict[Any, str]
) -> tuple[InterfacePort, tuple[Port, ...]]:
    if symbol.isGeneric:
        raise CodegenError(
            f"interface port {symbol.name!r} is generic; select a concrete "
            "interface type and modport"
        )
    if not symbol.modport:
        raise CodegenError(
            f"interface port {symbol.name!r} does not select a modport; "
            "cpptb requires a modport so testbench drive and sample "
            "directions are unambiguous"
        )

    connection = tuple(symbol.connection)
    if len(connection) < 2:
        raise CodegenError(
            f"interface port {symbol.name!r} has no elaborated modport "
            "connection"
        )
    instance = _first_interface_instance(pyslang, connection[0])
    modport = connection[1]
    if modport.kind != pyslang.ast.SymbolKind.Modport:
        raise CodegenError(
            f"interface port {symbol.name!r} selected object "
            f"{modport.name!r} is not a modport"
        )

    interface_dimensions = _declared_unpacked_ranges(symbol.declaredRange)
    constructor_ports: list[InterfaceConstructorPort] = []
    constructor_names: set[str] = set()
    for constructor in instance.body.portList:
        if constructor.kind != pyslang.ast.SymbolKind.Port:
            raise CodegenError(
                f"interface {symbol.interfaceDef.name!r} constructor port "
                f"{constructor.name!r} has unsupported kind {constructor.kind}"
            )
        direction = directions.get(constructor.direction)
        if direction not in {"input", "output", "inout"}:
            raise CodegenError(
                f"interface {symbol.interfaceDef.name!r} constructor port "
                f"{constructor.name!r} has unsupported direction "
                f"{constructor.direction}"
            )
        element_type, unpacked = _port_shape(constructor.type)
        if not element_type.isIntegral:
            raise CodegenError(
                f"interface {symbol.interfaceDef.name!r} constructor port "
                f"{constructor.name!r} is not packed integral"
            )
        constructor_ports.append(
            InterfaceConstructorPort(
                name=constructor.name,
                direction=direction,
                width=int(element_type.bitWidth),
                signed=bool(element_type.isSigned),
                four_state=bool(element_type.isFourState),
                unpacked=unpacked,
            )
        )
        constructor_names.add(constructor.name)

    parameters = tuple(
        InterfaceParameter(
            parameter.name,
            _resolved_parameter_text(parameter, symbol.name),
        )
        for parameter in instance.body.parameters
        if not parameter.isLocalParam
    )
    interface = InterfacePort(
        name=symbol.name,
        definition=symbol.interfaceDef.name,
        modport=symbol.modport,
        unpacked=interface_dimensions,
        parameters=parameters,
        constructor_ports=tuple(constructor_ports),
    )

    members: list[Port] = []
    seen_members: set[str] = set()
    for member in modport:
        if member.kind != pyslang.ast.SymbolKind.ModportPort:
            # Imported tasks, functions, and clocking blocks are not signal
            # transport members. They remain available to the HDL itself.
            continue
        if member.name in seen_members:
            raise CodegenError(
                f"interface port {symbol.name!r} modport {symbol.modport!r} "
                f"contains duplicate member {member.name!r}"
            )
        seen_members.add(member.name)
        direction = directions.get(member.direction)
        if direction is None:
            raise CodegenError(
                f"interface port {symbol.name!r} member {member.name!r} has "
                f"unsupported direction {member.direction}"
            )
        member_type, member_dimensions = _port_shape(member.type)
        packed_type = (
            _packed_type(member_type) if member_type.isIntegral else None
        )
        kind = (
            _transport_kind(packed_type)
            if packed_type is not None
            else _port_kind(member_type)
        )
        internal_name = member.internalSymbol.name
        members.append(
            Port(
                name=f"{symbol.name}.{member.name}",
                direction=direction,
                width=(int(member_type.bitWidth) if member_type.isIntegral else 0),
                cpp_path=(symbol.name, member.name),
                type_kind=kind,
                signed=bool(member_type.isSigned),
                four_state=bool(member_type.isFourState),
                unpacked=(*interface_dimensions, *member_dimensions),
                packed_type=packed_type,
                interface_name=symbol.name,
                interface_member=internal_name,
                interface_rank=len(interface_dimensions),
                interface_constructor_port=internal_name in constructor_names,
            )
        )
    if not members:
        raise CodegenError(
            f"interface port {symbol.name!r} modport {symbol.modport!r} has "
            "no signal members"
        )
    return interface, tuple(members)


class SlangFrontend:
    name = "slang"

    def elaborate(self, manifest: dict[str, Any], base_dir: Path) -> DesignIR:
        pyslang, compilation = _create_compilation(manifest, base_dir)

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
        interfaces: list[InterfacePort] = []
        for symbol in top.body.portList:
            if symbol.kind == pyslang.ast.SymbolKind.InterfacePort:
                interface, members = _interface_contract(
                    pyslang, symbol, directions
                )
                interfaces.append(interface)
                ports.extend(members)
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
            manifest["module"],
            tuple(ports),
            _resolve_internals(pyslang, top, manifest),
            _elaborate_hierarchy(pyslang, top),
            tuple(interfaces),
        )
