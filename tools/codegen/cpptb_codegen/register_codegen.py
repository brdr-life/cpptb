"""Generate cpptb register metadata and typed frontdoor handles from SystemRDL."""

from __future__ import annotations

import keyword
import re
from dataclasses import dataclass, field as dataclass_field, replace
from pathlib import Path
from typing import Iterable

from . import __version__


_CPP_KEYWORDS = {
    "alignas",
    "alignof",
    "and",
    "and_eq",
    "asm",
    "atomic_cancel",
    "atomic_commit",
    "atomic_noexcept",
    "auto",
    "bitand",
    "bitor",
    "bool",
    "break",
    "case",
    "catch",
    "char",
    "char8_t",
    "char16_t",
    "char32_t",
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
    "not_eq",
    "nullptr",
    "operator",
    "or",
    "or_eq",
    "private",
    "protected",
    "public",
    "reflexpr",
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
    "synchronized",
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
    "xor_eq",
}

_REGISTER_MODEL_METHOD_NAMES = {
    "descriptor",
    "for_each_memory",
    "for_each_register",
    "for_each_register_async",
    "mirror_all",
    "register_handles",
    "reset_all",
    "update_all",
}

_REGISTER_HANDLE_MEMBER_NAMES = {
    "Base",
    "address",
    "auto_predict",
    "descriptor",
    "staged",
    "staged_valid_mask",
    "end_address",
    "field",
    "for_each_field",
    "has_backdoor",
    "hdl_path",
    "hdl_slices",
    "is_transfer_address",
    "mirror",
    "mirrored",
    "mirrored_valid_mask",
    "name",
    "needs_update",
    "path",
    "peek",
    "poke",
    "predict",
    "predict_transfer_read",
    "predict_transfer_write",
    "predict_write",
    "read",
    "reset",
    "set_auto_predict",
    "stage",
    "update",
    "valid_byte_enable_mask",
    "width",
    "write",
}


def cpp_identifier(value: str) -> str:
    identifier = re.sub(r"[^A-Za-z0-9_]", "_", value)
    identifier = re.sub(r"_+", "_", identifier).strip("_") or "unnamed"
    if identifier[0].isdigit():
        identifier = f"n_{identifier}"
    if identifier in _CPP_KEYWORDS or keyword.iskeyword(identifier):
        identifier += "_"
    return identifier


def _cpp_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _enum_name(value: object | None) -> str:
    return getattr(value, "name", "") if value is not None else ""


_ACCESS = {
    "na": "cpptb::vc::RegisterAccess::None",
    "r": "cpptb::vc::RegisterAccess::ReadOnly",
    "w": "cpptb::vc::RegisterAccess::WriteOnly",
    "rw": "cpptb::vc::RegisterAccess::ReadWrite",
    "w1": "cpptb::vc::RegisterAccess::WriteOnce",
    "rw1": "cpptb::vc::RegisterAccess::ReadWriteOnce",
}

_READ_EFFECT = {
    "": "cpptb::vc::RegisterReadEffect::None",
    "rclr": "cpptb::vc::RegisterReadEffect::Clear",
    "rset": "cpptb::vc::RegisterReadEffect::Set",
    "ruser": "cpptb::vc::RegisterReadEffect::User",
}

_WRITE_EFFECT = {
    "": "cpptb::vc::RegisterWriteEffect::None",
    "woset": "cpptb::vc::RegisterWriteEffect::WriteOneSet",
    "woclr": "cpptb::vc::RegisterWriteEffect::WriteOneClear",
    "wot": "cpptb::vc::RegisterWriteEffect::WriteOneToggle",
    "wzs": "cpptb::vc::RegisterWriteEffect::WriteZeroSet",
    "wzc": "cpptb::vc::RegisterWriteEffect::WriteZeroClear",
    "wzt": "cpptb::vc::RegisterWriteEffect::WriteZeroToggle",
    "wclr": "cpptb::vc::RegisterWriteEffect::Clear",
    "wset": "cpptb::vc::RegisterWriteEffect::Set",
    "wuser": "cpptb::vc::RegisterWriteEffect::User",
}


class RegisterCodegenError(ValueError):
    """A SystemRDL contract cannot be represented by the generated model."""


def _mapped_property(
    mapping: dict[str, str], value: object | None, property_name: str, node: object
) -> str:
    name = _enum_name(value)
    try:
        return mapping[name]
    except KeyError as error:
        path = node.get_path()
        rendered = name or repr(value)
        raise RegisterCodegenError(
            f"{path}: unsupported SystemRDL {property_name} value {rendered}"
        ) from error


def _integer_reset(value: object | None, node: object) -> tuple[int, int]:
    if value is None:
        return 0, 0
    if not isinstance(value, int):
        raise RegisterCodegenError(
            f"{node.get_path()}: reset references are not supported; "
            "use an integer reset value"
        )
    width = int(node.width)
    return value, (1 << width) - 1


def _require_model_width(
    width: int, path: str, kind: str, *, maximum: int = 0xFFFF
) -> None:
    if width <= 0 or width > maximum:
        raise RegisterCodegenError(
            f"{path}: {kind} width {width} cannot be represented by the "
            f"current {maximum}-bit descriptor"
        )


@dataclass(frozen=True)
class EnumerationMember:
    name: str
    value: int


@dataclass(frozen=True)
class FieldEnumeration:
    name: str
    scope: str
    members: tuple[EnumerationMember, ...]


@dataclass(frozen=True)
class BackdoorSlice:
    path: str
    register_lsb: int
    width: int


@dataclass(frozen=True)
class Field:
    name: str
    path: str
    lsb: int
    width: int
    access: str
    read_effect: str
    write_effect: str
    reset_value: int
    reset_mask: int
    volatile: bool
    enumeration: FieldEnumeration | None = None
    backdoor_slices: tuple[BackdoorSlice, ...] = ()


@dataclass(frozen=True)
class Register:
    name: str
    member: str
    path: str
    address: int
    width: int
    access_width: int
    endianness: str
    reset_value: int
    reset_mask: int
    fields: tuple[Field, ...]
    backdoor_slices: tuple[BackdoorSlice, ...] = ()
    backdoor_complete: bool = False


@dataclass(frozen=True)
class Memory:
    name: str
    member: str
    path: str
    address: int
    entries: int
    width: int
    access_width: int
    access: str
    backdoor_path: str | None = None


@dataclass(frozen=True)
class RegisterBlock:
    name: str
    registers: tuple[Register, ...]
    memories: tuple[Memory, ...]


@dataclass
class RegisterViewNode:
    path: tuple[str, ...]
    children: dict[str, "RegisterViewNode"] = dataclass_field(
        default_factory=dict
    )
    register_index: int | None = None
    memory_index: int | None = None


def _build_register_view(block: RegisterBlock) -> RegisterViewNode:
    root = RegisterViewNode(())

    def insert(path: str, *, register: int | None = None,
               memory: int | None = None) -> None:
        components = tuple(path.split(".")[1:])
        node = root
        for component in components:
            node = node.children.setdefault(
                component, RegisterViewNode((*node.path, component))
            )
        node.register_index = register
        node.memory_index = memory

    for index, register in enumerate(block.registers):
        insert(register.path, register=index)
    for index, memory in enumerate(block.memories):
        insert(memory.path, memory=index)
    return root


def _view_type_name(path: tuple[str, ...]) -> str:
    return "RegisterScopeView_" + cpp_identifier("_".join(path))


def _view_child_type(node: RegisterViewNode, block: RegisterBlock) -> str:
    if node.register_index is not None:
        return f"Register{node.register_index}Handle<Master>&"
    if node.memory_index is not None:
        return "cpptb::vc::RegisterMemoryHandle<Master>&"
    return f"{_view_type_name(node.path)}<Master>"


def _view_child_expression(
    node: RegisterViewNode, block: RegisterBlock
) -> str:
    if node.register_index is not None:
        return block.registers[node.register_index].member
    if node.memory_index is not None:
        return block.memories[node.memory_index].member
    member_expressions = [
        expression
        for _, _, expression in _view_member_specs(node, block)
    ]
    return (
        f"{_view_type_name(node.path)}<Master>{{"
        + ", ".join(member_expressions)
        + "}"
    )


def _view_member_specs(
    node: RegisterViewNode, block: RegisterBlock
) -> list[tuple[str, str, str]]:
    scalar_children: list[tuple[str, RegisterViewNode]] = []
    array_children: dict[str, list[tuple[int, RegisterViewNode]]] = {}
    for component, child in node.children.items():
        match = re.fullmatch(r"(.+)\[(-?\d+)\]", component)
        if match:
            array_children.setdefault(match.group(1), []).append(
                (int(match.group(2)), child)
            )
        else:
            scalar_children.append((component, child))

    specs: list[tuple[str, str, str]] = []
    for component, child in sorted(scalar_children):
        specs.append(
            (
                cpp_identifier(component),
                _view_child_type(child, block),
                _view_child_expression(child, block),
            )
        )
    for base_name, entries in sorted(array_children.items()):
        ordered = sorted(entries)
        indices = [index for index, _ in ordered]
        if indices != list(range(len(indices))):
            rendered = ", ".join(str(index) for index in indices)
            raise RegisterCodegenError(
                f"register-model array {'.'.join((*node.path, base_name))} "
                f"has unsupported indices [{rendered}]; expected zero-based "
                "contiguous indices"
            )
        types = [_view_child_type(child, block) for _, child in ordered]
        expressions = [
            _view_child_expression(child, block) for _, child in ordered
        ]
        array_type = (
            "cpptb::vc::RegisterViewArray<" + ", ".join(types) + ">"
        )
        specs.append(
            (
                cpp_identifier(base_name),
                array_type,
                array_type + "{" + ", ".join(expressions) + "}",
            )
        )
    names = [name for name, _, _ in specs]
    if len(names) != len(set(names)):
        raise RegisterCodegenError(
            "register-model hierarchy members collide after C++ identifier "
            f"normalization in {'.'.join(node.path) or '<root>'}"
        )
    return specs


def _member_names(
    paths: Iterable[str], *, reserved: Iterable[str] = ()
) -> list[str]:
    used = set(reserved)
    result: list[str] = []
    for path in paths:
        pieces = path.split(".")[1:]
        base = cpp_identifier("_".join(pieces) or path)
        name = base
        suffix = 2
        while name in used:
            name = f"{base}_{suffix}"
            suffix += 1
        used.add(name)
        result.append(name)
    return result


def _effective_top(top_node: object) -> object:
    """Discard importer-created wrapper addrmaps around a single real block."""
    from systemrdl.node import AddrmapNode

    children = list(top_node.children(unroll=True))
    child = children[0] if len(children) == 1 else None
    importer_wrapper = (
        isinstance(child, AddrmapNode)
        and "__" in top_node.inst_name
    )
    if importer_wrapper:
        return child
    return top_node


def _normalized_path(node: object, block: object) -> str:
    raw_path = node.get_path()
    block_path = block.get_path()
    suffix = raw_path.removeprefix(block_path).lstrip(".")
    return block.inst_name if not suffix else f"{block.inst_name}.{suffix}"


def _join_hdl_path(parts: Iterable[str]) -> str:
    result = ""
    for part in parts:
        if not part:
            continue
        if not result:
            result = part
        elif part.startswith((".", "[")) or result.endswith("."):
            result += part
        else:
            result += f".{part}"
    return result


def _ancestor_hdl_parts(node: object, block: object) -> list[str]:
    from systemrdl.node import AddrmapNode, RegNode, RegfileNode

    parts: list[str] = []
    current = node
    while current is not None:
        if isinstance(current, (AddrmapNode, RegfileNode, RegNode)):
            value = current.get_property("hdl_path")
            if value:
                parts.append(str(value))
        if current == block:
            break
        current = current.parent
    parts.reverse()
    return parts


def _register_backdoor_slices(
    node: object, block: object, fields: list[Field]
) -> tuple[
    tuple[BackdoorSlice, ...], bool, dict[str, tuple[BackdoorSlice, ...]]
]:
    register_path = node.get_property("hdl_path")
    if register_path:
        path = _join_hdl_path(_ancestor_hdl_parts(node, block))
        return (
            (BackdoorSlice(path, 0, int(node.get_property("regwidth"))),),
            True,
            {
                field.name: (BackdoorSlice(path, field.lsb, field.width),)
                for field in fields
            },
        )

    slices: list[BackdoorSlice] = []
    field_slices: dict[str, tuple[BackdoorSlice, ...]] = {
        field.name: () for field in fields
    }
    complete = bool(fields)
    fields_by_name = {field.name: field for field in fields}
    for field_node in node.fields():
        field = fields_by_name[field_node.inst_name]
        path_slices = field_node.get_property("hdl_path_slice")
        if not path_slices:
            complete = False
            continue
        paths = [str(value) for value in path_slices]
        if len(paths) == 1:
            slice_ = BackdoorSlice(
                _join_hdl_path(
                    [*_ancestor_hdl_parts(node, block), paths[0]]
                ),
                field.lsb,
                field.width,
            )
            slices.append(slice_)
            field_slices[field.name] = (slice_,)
            continue
        if len(paths) != field.width:
            raise RegisterCodegenError(
                f"{field.path}: hdl_path_slice has {len(paths)} entries for "
                f"a {field.width}-bit field; use one path for the entire "
                "field or one path per bit from MSB to LSB"
            )
        bit_slices: list[BackdoorSlice] = []
        for index, path in enumerate(paths):
            bit_slices.append(
                BackdoorSlice(
                    _join_hdl_path(
                        [*_ancestor_hdl_parts(node, block), path]
                    ),
                    field.lsb + field.width - 1 - index,
                    1,
                )
            )
        slices.extend(bit_slices)
        field_slices[field.name] = tuple(bit_slices)
    return tuple(slices), complete, field_slices


def _memory_backdoor_path(node: object, block: object) -> str | None:
    path_slices = node.get_property("hdl_path_slice")
    if not path_slices:
        return None
    paths = [str(value) for value in path_slices]
    if len(paths) != 1:
        raise RegisterCodegenError(
            f"{_normalized_path(node, block)}: memory hdl_path_slice must "
            "contain one path naming the complete unpacked memory array"
        )
    return _join_hdl_path([*_ancestor_hdl_parts(node, block), paths[0]])


def _field_enumeration(
    field: object, width: int, path: str
) -> FieldEnumeration | None:
    encoded = field.get_property("encode")
    if encoded is None:
        return None
    if width > 64:
        raise RegisterCodegenError(
            f"{path}: encoded field width {width} cannot use the generated "
            "C++ enum API; encoded fields are limited to 64 bits"
        )
    members: list[EnumerationMember] = []
    maximum = (1 << width) - 1
    for member in encoded:
        value = int(member)
        if value < 0 or value > maximum:
            raise RegisterCodegenError(
                f"{path}: enum member {member.name}={value} does not fit "
                f"the {width}-bit encoded field"
            )
        members.append(EnumerationMember(member.name, value))
    if not members:
        raise RegisterCodegenError(f"{path}: encode enum has no members")
    return FieldEnumeration(
        name=str(encoded.type_name),
        scope=str(encoded.get_scope_path(".")),
        members=tuple(members),
    )


def extract_register_block(
    top_node: object, *, endianness: str = "little"
) -> RegisterBlock:
    """Convert an elaborated systemrdl-compiler tree into a stable small IR."""
    from systemrdl.node import MemNode, RegNode

    if endianness not in {"little", "big"}:
        raise RegisterCodegenError(
            f"unsupported register frontdoor endianness: {endianness}"
        )
    block_node = _effective_top(top_node)
    register_nodes = sorted(
        (
            node
            for node in block_node.descendants(unroll=True)
            if isinstance(node, RegNode)
        ),
        key=lambda node: (int(node.absolute_address), node.get_path()),
    )
    register_paths = [
        _normalized_path(node, block_node) for node in register_nodes
    ]
    members = _member_names(
        register_paths, reserved=_REGISTER_MODEL_METHOD_NAMES
    )
    registers: list[Register] = []
    for node, path, member in zip(
        register_nodes, register_paths, members, strict=True
    ):
        if node.is_alias:
            raise RegisterCodegenError(
                f"{path}: SystemRDL register aliases are not yet supported; "
                "use a RegisterAddressMap alias explicitly so all views share "
                "one logical mirror"
            )
        register_width = int(node.get_property("regwidth"))
        access_width = int(node.get_property("accesswidth"))
        _require_model_width(register_width, path, "register")
        if access_width <= 0 or access_width > register_width:
            raise RegisterCodegenError(
                f"{path}: invalid accesswidth {access_width} for regwidth "
                f"{register_width}"
            )
        fields: list[Field] = []
        reset_value = 0
        reset_mask = 0
        for field in node.fields():
            width = int(field.width)
            lsb = int(field.low)
            field_path = _normalized_path(field, block_node)
            reset = field.get_property("reset")
            field_reset, field_mask = _integer_reset(reset, field)
            if field_reset >> 64 or field_mask >> 64:
                raise RegisterCodegenError(
                    f"{field_path}: reset metadata for fields wider than 64 "
                    "bits is not representable"
                )
            reset_value |= field_reset << lsb
            reset_mask |= field_mask << lsb
            fields.append(
                Field(
                    name=field.inst_name,
                    path=field_path,
                    lsb=lsb,
                    width=width,
                    access=_mapped_property(
                        _ACCESS, field.get_property("sw"), "sw", field
                    ),
                    read_effect=_mapped_property(
                        _READ_EFFECT,
                        field.get_property("onread"),
                        "onread",
                        field,
                    ),
                    write_effect=_mapped_property(
                        _WRITE_EFFECT,
                        field.get_property("onwrite"),
                        "onwrite",
                        field,
                    ),
                    reset_value=field_reset,
                    reset_mask=field_mask,
                    volatile=bool(field.is_volatile),
                    enumeration=_field_enumeration(field, width, field_path),
                )
            )
        backdoor_slices, backdoor_complete, field_backdoor_slices = (
            _register_backdoor_slices(node, block_node, fields)
        )
        fields = [
            replace(
                field,
                backdoor_slices=field_backdoor_slices[field.name],
            )
            for field in fields
        ]
        registers.append(
            Register(
                name=node.inst_name,
                member=member,
                path=path,
                address=int(node.absolute_address),
                width=register_width,
                access_width=access_width,
                endianness=(
                    "cpptb::vc::RegisterEndianness::Little"
                    if endianness == "little"
                    else "cpptb::vc::RegisterEndianness::Big"
                ),
                reset_value=reset_value,
                reset_mask=reset_mask,
                fields=tuple(fields),
                backdoor_slices=backdoor_slices,
                backdoor_complete=backdoor_complete,
            )
        )

    memory_nodes = sorted(
        (
            node
            for node in block_node.descendants(unroll=True)
            if isinstance(node, MemNode)
        ),
        key=lambda node: (int(node.absolute_address), node.get_path()),
    )
    used_members = {*members, *_REGISTER_MODEL_METHOD_NAMES}
    memory_members: list[str] = []
    for node in memory_nodes:
        path = _normalized_path(node, block_node)
        base = _member_names([path])[0]
        member = base
        suffix = 2
        while member in used_members:
            member = f"{base}_{suffix}"
            suffix += 1
        used_members.add(member)
        memory_members.append(member)

    memories: list[Memory] = []
    for node, member in zip(memory_nodes, memory_members, strict=True):
        path = _normalized_path(node, block_node)
        memory_width = int(node.get_property("memwidth"))
        _require_model_width(memory_width, path, "memory element")
        if node.is_sw_readable and node.is_sw_writable:
            access = "cpptb::vc::RegisterAccess::ReadWrite"
        elif node.is_sw_readable:
            access = "cpptb::vc::RegisterAccess::ReadOnly"
        elif node.is_sw_writable:
            access = "cpptb::vc::RegisterAccess::WriteOnly"
        else:
            access = "cpptb::vc::RegisterAccess::None"
        memories.append(
            Memory(
                name=node.inst_name,
                member=member,
                path=path,
                address=int(node.absolute_address),
                entries=int(node.get_property("mementries")),
                width=memory_width,
                access_width=min(memory_width, 64),
                access=access,
                backdoor_path=_memory_backdoor_path(node, block_node),
            )
        )
    return RegisterBlock(
        name=block_node.inst_name,
        registers=tuple(registers),
        memories=tuple(memories),
    )


def _hdl_path_selection(path: str) -> tuple[str, str, int | None, int | None]:
    range_match = re.fullmatch(r"(.+)\[(-?\d+):(-?\d+)\]", path)
    if range_match:
        return (
            range_match.group(1),
            "range",
            int(range_match.group(2)),
            int(range_match.group(3)),
        )
    index_match = re.fullmatch(r"(.+)\[(-?\d+)\]", path)
    if index_match:
        return index_match.group(1), "select", int(index_match.group(2)), None
    return path, "full", None, None


def _backdoor_read_expression(slice_: BackdoorSlice, signal_name: str) -> str:
    _, selection, first, second = _hdl_path_selection(slice_.path)
    if selection == "range":
        return (
            "cpptb::vc::register_detail::read_hdl_range<"
            f"{first}, {second}, {slice_.width}>({signal_name})"
        )
    if selection == "select":
        return (
            "cpptb::vc::register_detail::read_hdl_select<"
            f"{first}, {slice_.width}>({signal_name})"
        )
    return (
        "cpptb::vc::register_detail::read_hdl_full<"
        f"{slice_.width}>({signal_name})"
    )


def _backdoor_write_statement(
    slice_: BackdoorSlice, signal_name: str, value_expression: str
) -> str:
    _, selection, first, second = _hdl_path_selection(slice_.path)
    if selection == "range":
        function = (
            "cpptb::vc::register_detail::write_hdl_range<"
            f"{first}, {second}, {slice_.width}>"
        )
    elif selection == "select":
        function = (
            "cpptb::vc::register_detail::write_hdl_select<"
            f"{first}, {slice_.width}>"
        )
    else:
        function = (
            "cpptb::vc::register_detail::write_hdl_full<"
            f"{slice_.width}>"
        )
    return f"{function}({signal_name}, {value_expression});"


def _wide_backdoor_read_statement(
    slice_: BackdoorSlice, signal_name: str
) -> str:
    _, selection, first, second = _hdl_path_selection(slice_.path)
    if selection == "range":
        function = (
            "cpptb::vc::register_detail::read_hdl_range_words<"
            f"{first}, {second}, {slice_.width}>"
        )
    elif selection == "select":
        function = (
            "cpptb::vc::register_detail::read_hdl_select_words<"
            f"{first}, {slice_.width}>"
        )
    else:
        function = (
            "cpptb::vc::register_detail::read_hdl_full_words<"
            f"{slice_.width}>"
        )
    return f"{function}({signal_name}, words, {slice_.register_lsb});"


def _wide_backdoor_write_statement(
    slice_: BackdoorSlice, signal_name: str
) -> str:
    _, selection, first, second = _hdl_path_selection(slice_.path)
    if selection == "range":
        function = (
            "cpptb::vc::register_detail::write_hdl_range_words<"
            f"{first}, {second}, {slice_.width}>"
        )
    elif selection == "select":
        function = (
            "cpptb::vc::register_detail::write_hdl_select_words<"
            f"{first}, {slice_.width}>"
        )
    else:
        function = (
            "cpptb::vc::register_detail::write_hdl_full_words<"
            f"{slice_.width}>"
        )
    return f"{function}({signal_name}, words, {slice_.register_lsb});"


def render_cpp(block: RegisterBlock, namespace: str, class_name: str) -> str:
    view_root = _build_register_view(block)
    has_wide_registers = any(register.width > 64 for register in block.registers)
    has_wide_memories = any(memory.width > 64 for memory in block.memories)
    view_nodes: list[RegisterViewNode] = []

    def collect_view_nodes(node: RegisterViewNode) -> None:
        for child in node.children.values():
            collect_view_nodes(child)
        if (
            node.path
            and node.children
            and node.register_index is None
            and node.memory_index is None
        ):
            view_nodes.append(node)

    collect_view_nodes(view_root)
    view_type_paths: dict[str, tuple[str, ...]] = {}
    for node in view_nodes:
        type_name = _view_type_name(node.path)
        previous = view_type_paths.setdefault(type_name, node.path)
        if previous != node.path:
            raise RegisterCodegenError(
                "register-model hierarchy type names collide after C++ "
                f"identifier normalization: {'.'.join(previous)} and "
                f"{'.'.join(node.path)}"
            )
    enum_cpp_names: dict[FieldEnumeration, str] = {}
    used_enum_names = {
        cpp_identifier(class_name),
        "DutBackdoor",
        "descriptor",
        "make_backdoor",
        "memories",
        "registers",
        *(f"Register{index}Handle" for index in range(len(block.registers))),
        *(_view_type_name(node.path) for node in view_nodes),
    }
    for register in block.registers:
        for field in register.fields:
            enumeration = field.enumeration
            if enumeration is None or enumeration in enum_cpp_names:
                continue
            base = cpp_identifier(enumeration.name)
            name = base
            suffix = 2
            while name in used_enum_names:
                name = f"{base}_{suffix}"
                suffix += 1
            used_enum_names.add(name)
            enum_cpp_names[enumeration] = name
    enum_member_cpp_names = {
        enumeration: _member_names(
            [f"enum.{member.name}" for member in enumeration.members]
        )
        for enumeration in enum_cpp_names
    }
    field_member_cpp_names = [
        _member_names(
            [f"register.{field.name}" for field in register.fields],
            reserved=_REGISTER_HANDLE_MEMBER_NAMES,
        )
        for register in block.registers
    ]
    root_scalar_leaves = {
        cpp_identifier(component)
        for component, child in view_root.children.items()
        if re.fullmatch(r"(.+)\[(-?\d+)\]", component) is None
        and (child.register_index is not None or child.memory_index is not None)
    }
    root_view_specs = [
        spec
        for spec in _view_member_specs(view_root, block)
        if spec[0] not in root_scalar_leaves
    ]

    lines = [
        f"// Generated by cpptb-codegen {__version__}. Do not edit.",
        "#pragma once",
        "",
        "#include <algorithm>",
        "#include <array>",
        "#include <concepts>",
        "#include <cstdint>",
        "#include <span>",
        "#include <string_view>",
        "#include <utility>",
        "",
        '#include "cpptb_vc/register_model.hpp"',
        "",
        f"namespace {namespace} {{",
        "",
    ]
    for enumeration, enum_name in enum_cpp_names.items():
        lines.extend(
            [
                f"enum class {enum_name} : uint64_t {{",
                *[
                    f"    {member_name} = "
                    f"UINT64_C(0x{member.value:x}),"
                    for member, member_name in zip(
                        enumeration.members,
                        enum_member_cpp_names[enumeration],
                        strict=True,
                    )
                ],
                "};",
                "",
                "[[nodiscard]] inline constexpr std::string_view "
                f"cpptb_diagnostic_name({enum_name} value) noexcept {{",
                "    switch (value) {",
                *[
                    f"        case {enum_name}::{member_name}: "
                    f"return {_cpp_string(enum_name + '::' + member_name)};"
                    for member, member_name in zip(
                        enumeration.members,
                        enum_member_cpp_names[enumeration],
                        strict=True,
                    )
                ],
                "    }",
                '    return "";',
                "}",
                "",
            ]
        )
    for index, register in enumerate(block.registers):
        if register.width > 64:
            word_count = (register.width + 31) // 32
            reset_words = ", ".join(
                f"UINT32_C(0x{(register.reset_value >> (word * 32)) & 0xffffffff:x})"
                for word in range(word_count)
            )
            mask_words = ", ".join(
                f"UINT32_C(0x{(register.reset_mask >> (word * 32)) & 0xffffffff:x})"
                for word in range(word_count)
            )
            lines.extend(
                [
                    f"inline constexpr std::array<uint32_t, {word_count}> "
                    f"register_{index}_reset_value_words{{{{{reset_words}}}}};",
                    f"inline constexpr std::array<uint32_t, {word_count}> "
                    f"register_{index}_reset_mask_words{{{{{mask_words}}}}};",
                    "",
                ]
            )
        for field_index, field in enumerate(register.fields):
            field_backdoor_array = (
                f"register_{index}_field_{field_index}_backdoor_slices"
            )
            lines.append(
                "inline constexpr std::array<"
                "cpptb::vc::RegisterBackdoorSliceDescriptor, "
                f"{len(field.backdoor_slices)}> {field_backdoor_array}{{{{"
            )
            for slice_ in field.backdoor_slices:
                lines.extend(
                    [
                        "    cpptb::vc::RegisterBackdoorSliceDescriptor{",
                        f"        .path = {_cpp_string(slice_.path)},",
                        f"        .register_lsb = {slice_.register_lsb},",
                        f"        .width = {slice_.width},",
                        "    },",
                    ]
                )
            lines.extend(["}};", ""])

        field_array = f"register_{index}_fields"
        lines.append(
            f"inline constexpr std::array<cpptb::vc::RegisterFieldDescriptor, "
            f"{len(register.fields)}> {field_array}{{{{"
        )
        for field_index, field in enumerate(register.fields):
            lines.extend(
                [
                    "    cpptb::vc::RegisterFieldDescriptor{",
                    f"        .name = {_cpp_string(field.name)},",
                    f"        .path = {_cpp_string(field.path)},",
                    f"        .lsb = {field.lsb},",
                    f"        .width = {field.width},",
                    f"        .access = {field.access},",
                    f"        .read_effect = {field.read_effect},",
                    f"        .write_effect = {field.write_effect},",
                    f"        .reset_value = UINT64_C(0x{field.reset_value & ((1 << 64) - 1):x}),",
                    f"        .reset_mask = UINT64_C(0x{field.reset_mask & ((1 << 64) - 1):x}),",
                    f"        .volatile_value = {'true' if field.volatile else 'false'},",
                    "        .backdoor_slices = "
                    f"register_{index}_field_{field_index}_backdoor_slices,",
                    "    },",
                ]
            )
        lines.extend(["}};", ""])

        backdoor_array = f"register_{index}_backdoor_slices"
        lines.append(
            "inline constexpr std::array<"
            "cpptb::vc::RegisterBackdoorSliceDescriptor, "
            f"{len(register.backdoor_slices)}> {backdoor_array}{{{{"
        )
        for slice_ in register.backdoor_slices:
            lines.extend(
                [
                    "    cpptb::vc::RegisterBackdoorSliceDescriptor{",
                    f"        .path = {_cpp_string(slice_.path)},",
                    f"        .register_lsb = {slice_.register_lsb},",
                    f"        .width = {slice_.width},",
                    "    },",
                ]
            )
        lines.extend(["}};", ""])

    lines.append(
        f"inline constexpr std::array<cpptb::vc::RegisterDescriptor, "
        f"{len(block.registers)}> registers{{{{"
    )
    for index, register in enumerate(block.registers):
        lines.extend(
            [
                "    cpptb::vc::RegisterDescriptor{",
                f"        .name = {_cpp_string(register.name)},",
                f"        .path = {_cpp_string(register.path)},",
                f"        .address = UINT64_C(0x{register.address:x}),",
                f"        .width = {register.width},",
                f"        .access_width = {register.access_width},",
                f"        .endianness = {register.endianness},",
                f"        .reset_value = UINT64_C(0x{register.reset_value & ((1 << 64) - 1):x}),",
                f"        .reset_mask = UINT64_C(0x{register.reset_mask & ((1 << 64) - 1):x}),",
                *(
                    [
                        f"        .reset_value_words = register_{index}_reset_value_words,",
                        f"        .reset_mask_words = register_{index}_reset_mask_words,",
                    ]
                    if register.width > 64
                    else []
                ),
                f"        .fields = register_{index}_fields,",
                f"        .backdoor_slices = register_{index}_backdoor_slices,",
                "    },",
            ]
        )
    lines.extend(["}};", ""])

    lines.append(
        f"inline constexpr std::array<cpptb::vc::RegisterMemoryDescriptor, "
        f"{len(block.memories)}> memories{{{{"
    )
    for memory in block.memories:
        lines.extend(
            [
                "    cpptb::vc::RegisterMemoryDescriptor{",
                f"        .name = {_cpp_string(memory.name)},",
                f"        .path = {_cpp_string(memory.path)},",
                f"        .address = UINT64_C(0x{memory.address:x}),",
                f"        .entries = UINT64_C({memory.entries}),",
                f"        .width = {memory.width},",
                f"        .access_width = {memory.access_width},",
                f"        .access = {memory.access},",
                f"        .hdl_path = {_cpp_string(memory.backdoor_path or '')},",
                "    },",
            ]
        )
    lines.extend(
        [
            "}};",
            "",
            "inline constexpr cpptb::vc::RegisterBlockDescriptor descriptor{",
            f"    .name = {_cpp_string(block.name)},",
            "    .address_unit_bits = 8,",
            "    .registers = registers,",
            "    .memories = memories,",
            "};",
            "",
        ]
    )

    lines.extend(
        [
            "template <cpptb::vc::MemoryMappedMaster Master, typename Dut>",
            "    requires std::unsigned_integral<typename Master::data_type>",
            "class DutBackdoor final",
            "    : public cpptb::vc::RegisterBackdoor<uint64_t>,",
        ]
    )
    if has_wide_registers:
        lines.append("      public cpptb::vc::WideRegisterBackdoor,")
    if has_wide_memories:
        lines.append("      public cpptb::vc::WideRegisterMemoryBackdoor,")
    lines.extend(
        [
            "      public cpptb::vc::RegisterMemoryBackdoor<",
            "          typename Master::data_type> {",
            "   public:",
            "    using data_type = uint64_t;",
            "    using memory_data_type = typename Master::data_type;",
            "",
            "    explicit DutBackdoor(Dut dut) : dut_(std::move(dut)) {}",
            "",
            "    data_type peek(const cpptb::vc::RegisterDescriptor& selected,",
            "                   uint64_t effective_address) override {",
            "        (void)effective_address;",
        ]
    )
    for index, register in enumerate(block.registers):
        if register.width > 64 or not register.backdoor_complete:
            continue
        lines.extend(
            [
                f"        if (&selected == &registers[{index}]) {{",
                "            uint64_t value = 0;",
            ]
        )
        for slice_index, slice_ in enumerate(register.backdoor_slices):
            lookup_path, _, _, _ = _hdl_path_selection(slice_.path)
            signal_name = f"signal_{slice_index}"
            lines.extend(
                [
                    f"            const auto {signal_name} = dut_.template "
                    f"cpptb_signal<{_cpp_string(lookup_path)}>();",
                    f"            value |= ({_backdoor_read_expression(slice_, signal_name)} "
                    f"& cpptb::vc::register_mask({slice_.width})) "
                    f"<< {slice_.register_lsb};",
                ]
            )
        lines.extend(
            [
                "            return static_cast<data_type>(value);",
                "        }",
            ]
        )
    lines.extend(
        [
            "        throw std::logic_error(",
            '            "cpptb-vc: generated RTL backdoor has no complete HDL path for: " +',
            "            std::string{selected.path});",
            "    }",
            "",
            "    void poke(const cpptb::vc::RegisterDescriptor& selected,",
            "              uint64_t effective_address, data_type value) override {",
            "        (void)effective_address;",
        ]
    )
    for index, register in enumerate(block.registers):
        if register.width > 64 or not register.backdoor_complete:
            continue
        lines.append(f"        if (&selected == &registers[{index}]) {{")
        for slice_index, slice_ in enumerate(register.backdoor_slices):
            lookup_path, _, _, _ = _hdl_path_selection(slice_.path)
            signal_name = f"signal_{slice_index}"
            value_expression = (
                f"(static_cast<uint64_t>(value) >> {slice_.register_lsb}) "
                f"& cpptb::vc::register_mask({slice_.width})"
            )
            lines.extend(
                [
                    f"            const auto {signal_name} = dut_.template "
                    f"cpptb_signal<{_cpp_string(lookup_path)}>();",
                    "            "
                    + _backdoor_write_statement(
                        slice_, signal_name, value_expression
                    ),
                ]
            )
        lines.extend(["            return;", "        }"])
    if has_wide_registers:
        lines.extend(
            [
                "        throw std::logic_error(",
                '            "cpptb-vc: generated RTL backdoor has no complete HDL path for: " +',
                "            std::string{selected.path});",
                "    }",
                "",
                "    void peek_words(",
                "        const cpptb::vc::RegisterDescriptor& selected,",
                "        uint64_t effective_address,",
                "        std::span<uint32_t> words) override {",
                "        (void)effective_address;",
                "        std::fill(words.begin(), words.end(), uint32_t{0});",
            ]
        )
        for index, register in enumerate(block.registers):
            if register.width <= 64 or not register.backdoor_complete:
                continue
            lines.append(f"        if (&selected == &registers[{index}]) {{")
            for slice_index, slice_ in enumerate(register.backdoor_slices):
                lookup_path, _, _, _ = _hdl_path_selection(slice_.path)
                signal_name = f"signal_{slice_index}"
                lines.extend(
                    [
                        f"            const auto {signal_name} = dut_.template "
                        f"cpptb_signal<{_cpp_string(lookup_path)}>();",
                        "            "
                        + _wide_backdoor_read_statement(slice_, signal_name),
                    ]
                )
            lines.extend(["            return;", "        }"])
        lines.extend(
            [
                "        throw std::logic_error(",
                '            "cpptb-vc: generated RTL wide backdoor has no complete HDL path for: " +',
                "            std::string{selected.path});",
                "    }",
                "",
                "    void poke_words(",
                "        const cpptb::vc::RegisterDescriptor& selected,",
                "        uint64_t effective_address,",
                "        std::span<const uint32_t> words) override {",
                "        (void)effective_address;",
            ]
        )
        for index, register in enumerate(block.registers):
            if register.width <= 64 or not register.backdoor_complete:
                continue
            lines.append(f"        if (&selected == &registers[{index}]) {{")
            for slice_index, slice_ in enumerate(register.backdoor_slices):
                lookup_path, _, _, _ = _hdl_path_selection(slice_.path)
                signal_name = f"signal_{slice_index}"
                lines.extend(
                    [
                        f"            const auto {signal_name} = dut_.template "
                        f"cpptb_signal<{_cpp_string(lookup_path)}>();",
                        "            "
                        + _wide_backdoor_write_statement(slice_, signal_name),
                    ]
                )
            lines.extend(["            return;", "        }"])
    lines.extend(
        [
            "        throw std::logic_error(",
            '            "cpptb-vc: generated RTL backdoor has no complete HDL path for: " +',
            "            std::string{selected.path});",
            "    }",
            "",
            "    memory_data_type peek(",
            "        const cpptb::vc::RegisterMemoryDescriptor& selected,",
            "        uint64_t index, uint64_t effective_address) override {",
            "        (void)effective_address;",
        ]
    )
    for index, memory in enumerate(block.memories):
        if memory.width > 64 or memory.backdoor_path is None:
            continue
        lines.extend(
            [
                f"        if (&selected == &memories[{index}]) {{",
                "            const auto memory = dut_.template ",
                f"                cpptb_signal<{_cpp_string(memory.backdoor_path)}>();",
                f"            static_assert(decltype(memory)::width == {memory.width},",
                '                          "SystemRDL memory width does not match the HDL array");',
                f"            static_assert(decltype(memory)::size >= {memory.entries},",
                '                          "SystemRDL memory has more entries than the HDL array");',
                "            const auto hdl_index = static_cast<int32_t>(",
                "                static_cast<int64_t>(decltype(memory)::low) +",
                "                static_cast<int64_t>(index));",
                "            return static_cast<memory_data_type>(",
                "                cpptb::vc::register_detail::hdl_value_to_uint64(",
                "                    memory.at(hdl_index).get()));",
                "        }",
            ]
        )
    if has_wide_memories:
        lines.extend(
            [
                "        throw std::logic_error(",
                '            "cpptb-vc: generated RTL backdoor has no complete HDL path for memory: " +',
                "            std::string{selected.path});",
                "    }",
                "",
                "    void peek_words(",
                "        const cpptb::vc::RegisterMemoryDescriptor& selected,",
                "        uint64_t index, uint64_t effective_address,",
                "        std::span<uint32_t> words) override {",
                "        (void)effective_address;",
                "        std::fill(words.begin(), words.end(), uint32_t{0});",
            ]
        )
        for index, memory in enumerate(block.memories):
            if memory.width <= 64 or memory.backdoor_path is None:
                continue
            lines.extend(
                [
                    f"        if (&selected == &memories[{index}]) {{",
                    "            const auto memory = dut_.template ",
                    f"                cpptb_signal<{_cpp_string(memory.backdoor_path)}>();",
                    f"            static_assert(decltype(memory)::width == {memory.width},",
                    '                          "SystemRDL memory width does not match the HDL array");',
                    f"            static_assert(decltype(memory)::size >= {memory.entries},",
                    '                          "SystemRDL memory has more entries than the HDL array");',
                    "            const auto hdl_index = static_cast<int32_t>(",
                    "                static_cast<int64_t>(decltype(memory)::low) +",
                    "                static_cast<int64_t>(index));",
                    "            const auto element = memory.at(hdl_index);",
                    f"            cpptb::vc::register_detail::read_hdl_full_words<{memory.width}>(",
                    "                element, words, 0);",
                    "            return;",
                    "        }",
                ]
            )
        lines.extend(
            [
                "        throw std::logic_error(",
                '            "cpptb-vc: generated RTL wide backdoor has no complete HDL path for memory: " +',
                "            std::string{selected.path});",
                "    }",
                "",
                "    void poke_words(",
                "        const cpptb::vc::RegisterMemoryDescriptor& selected,",
                "        uint64_t index, uint64_t effective_address,",
                "        std::span<const uint32_t> words) override {",
                "        (void)effective_address;",
            ]
        )
        for index, memory in enumerate(block.memories):
            if memory.width <= 64 or memory.backdoor_path is None:
                continue
            lines.extend(
                [
                    f"        if (&selected == &memories[{index}]) {{",
                    "            const auto memory = dut_.template ",
                    f"                cpptb_signal<{_cpp_string(memory.backdoor_path)}>();",
                    f"            static_assert(decltype(memory)::width == {memory.width},",
                    '                          "SystemRDL memory width does not match the HDL array");',
                    f"            static_assert(decltype(memory)::size >= {memory.entries},",
                    '                          "SystemRDL memory has more entries than the HDL array");',
                    "            const auto hdl_index = static_cast<int32_t>(",
                    "                static_cast<int64_t>(decltype(memory)::low) +",
                    "                static_cast<int64_t>(index));",
                    "            const auto element = memory.at(hdl_index);",
                    f"            cpptb::vc::register_detail::write_hdl_full_words<{memory.width}>(",
                    "                element, words, 0);",
                    "            return;",
                    "        }",
                ]
            )
    lines.extend(
        [
            "        throw std::logic_error(",
            '            "cpptb-vc: generated RTL backdoor has no complete HDL path for memory: " +',
            "            std::string{selected.path});",
            "    }",
            "",
            "    void poke(",
            "        const cpptb::vc::RegisterMemoryDescriptor& selected,",
            "        uint64_t index, uint64_t effective_address,",
            "        memory_data_type value) override {",
            "        (void)effective_address;",
        ]
    )
    for index, memory in enumerate(block.memories):
        if memory.width > 64 or memory.backdoor_path is None:
            continue
        lines.extend(
            [
                f"        if (&selected == &memories[{index}]) {{",
                "            const auto memory = dut_.template ",
                f"                cpptb_signal<{_cpp_string(memory.backdoor_path)}>();",
                f"            static_assert(decltype(memory)::width == {memory.width},",
                '                          "SystemRDL memory width does not match the HDL array");',
                f"            static_assert(decltype(memory)::size >= {memory.entries},",
                '                          "SystemRDL memory has more entries than the HDL array");',
                "            const auto hdl_index = static_cast<int32_t>(",
                "                static_cast<int64_t>(decltype(memory)::low) +",
                "                static_cast<int64_t>(index));",
                "            memory.at(hdl_index).deposit(",
                "                cpptb::vc::register_detail::hdl_value_from_uint64<",
                "                    decltype(memory)>(value));",
                "            return;",
                "        }",
            ]
        )
    lines.extend(
        [
            "        throw std::logic_error(",
            '            "cpptb-vc: generated RTL backdoor has no complete HDL path for memory: " +',
            "            std::string{selected.path});",
            "    }",
            "",
            "    void peek_into(",
            "        const cpptb::vc::RegisterMemoryDescriptor& selected,",
            "        uint64_t first_index, uint64_t first_effective_address,",
            "        std::span<memory_data_type> values) override {",
            "        (void)first_effective_address;",
        ]
    )
    for index, memory in enumerate(block.memories):
        if memory.width > 64 or memory.backdoor_path is None:
            continue
        lines.extend(
            [
                f"        if (&selected == &memories[{index}]) {{",
                "            const auto memory = dut_.template ",
                f"                cpptb_signal<{_cpp_string(memory.backdoor_path)}>();",
                f"            static_assert(decltype(memory)::width == {memory.width},",
                '                          "SystemRDL memory width does not match the HDL array");',
                f"            static_assert(decltype(memory)::size >= {memory.entries},",
                '                          "SystemRDL memory has more entries than the HDL array");',
                "            const auto hdl_index = static_cast<int32_t>(",
                "                static_cast<int64_t>(decltype(memory)::low) +",
                "                static_cast<int64_t>(first_index));",
                "            if constexpr (",
                "                std::same_as<typename decltype(memory)::value_type,",
                "                             memory_data_type> &&",
                "                requires(decltype(memory) candidate,",
                "                         std::span<memory_data_type> output) {",
                "                    candidate.get_into(int32_t{}, output);",
                "                }) {",
                "                memory.get_into(hdl_index, values);",
                "            } else {",
                "                for (std::size_t offset = 0;",
                "                     offset < values.size(); ++offset) {",
                "                    values[offset] = static_cast<memory_data_type>(",
                "                        cpptb::vc::register_detail::hdl_value_to_uint64(",
                "                            memory.at(hdl_index +",
                "                                      static_cast<int32_t>(offset)).get()));",
                "                }",
                "            }",
                "            return;",
                "        }",
            ]
        )
    lines.extend(
        [
            "        throw std::logic_error(",
            '            "cpptb-vc: generated RTL backdoor has no complete HDL path for memory: " +',
            "            std::string{selected.path});",
            "    }",
            "",
            "    void poke(",
            "        const cpptb::vc::RegisterMemoryDescriptor& selected,",
            "        uint64_t first_index, uint64_t first_effective_address,",
            "        std::span<const memory_data_type> values) override {",
            "        (void)first_effective_address;",
        ]
    )
    for index, memory in enumerate(block.memories):
        if memory.width > 64 or memory.backdoor_path is None:
            continue
        lines.extend(
            [
                f"        if (&selected == &memories[{index}]) {{",
                "            const auto memory = dut_.template ",
                f"                cpptb_signal<{_cpp_string(memory.backdoor_path)}>();",
                f"            static_assert(decltype(memory)::width == {memory.width},",
                '                          "SystemRDL memory width does not match the HDL array");',
                f"            static_assert(decltype(memory)::size >= {memory.entries},",
                '                          "SystemRDL memory has more entries than the HDL array");',
                "            const auto hdl_index = static_cast<int32_t>(",
                "                static_cast<int64_t>(decltype(memory)::low) +",
                "                static_cast<int64_t>(first_index));",
                "            if constexpr (",
                "                std::same_as<typename decltype(memory)::value_type,",
                "                             memory_data_type> &&",
                "                requires(decltype(memory) candidate,",
                "                         std::span<const memory_data_type> input) {",
                "                    candidate.deposit(int32_t{}, input);",
                "                }) {",
                "                memory.deposit(hdl_index, values);",
                "            } else {",
                "                for (std::size_t offset = 0;",
                "                     offset < values.size(); ++offset) {",
                "                    memory.at(hdl_index +",
                "                              static_cast<int32_t>(offset)).deposit(",
                "                        cpptb::vc::register_detail::hdl_value_from_uint64<",
                "                            decltype(memory)>(values[offset]));",
                "                }",
                "            }",
                "            return;",
                "        }",
            ]
        )
    lines.extend(
        [
            "        throw std::logic_error(",
            '            "cpptb-vc: generated RTL backdoor has no complete HDL path for memory: " +',
            "            std::string{selected.path});",
            "    }",
            "",
            "   private:",
            "    Dut dut_;",
            "};",
            "",
            "template <cpptb::vc::MemoryMappedMaster Master, typename Dut>",
            "[[nodiscard]] auto make_backdoor(Dut dut) {",
            "    return DutBackdoor<Master, Dut>{std::move(dut)};",
            "}",
            "",
        ]
    )

    for index, register in enumerate(block.registers):
        type_name = f"Register{index}Handle"
        base_type = (
            f"cpptb::vc::WideRegisterHandle<{register.width}, Master>"
            if register.width > 64
            else "cpptb::vc::RegisterHandle<Master>"
        )
        lines.extend(
            [
                "template <cpptb::vc::MemoryMappedMaster Master>",
                "    requires std::unsigned_integral<typename Master::data_type>",
                f"class {type_name} : public {base_type} {{",
                "   public:",
                f"    using Base = {base_type};",
            ]
        )
        for field, field_member_name in zip(
            register.fields, field_member_cpp_names[index], strict=True
        ):
            raw_field_type = (
                "cpptb::vc::WideRegisterFieldHandle<"
                f"{register.width}, {field.width}, Master>"
                if register.width > 64
                else "cpptb::vc::RegisterFieldHandle<Master>"
            )
            field_type = (
                "cpptb::vc::RegisterEnumFieldHandle<"
                f"{enum_cpp_names[field.enumeration]}, {raw_field_type}>"
                if field.enumeration is not None
                else raw_field_type
            )
            lines.append(f"    {field_type} {field_member_name};")
        lines.extend(
            [
                "",
                f"    {type_name}(cpptb::TestContext test, Master& master,",
                "                    uint64_t base_address,",
                (
                    "                    cpptb::vc::WideRegisterBackdoor* backdoor,"
                    if register.width > 64
                    else "                    cpptb::vc::RegisterBackdoor<uint64_t>* backdoor,"
                ),
                "                    cpptb::vc::RegisterUserEffectPolicy* user_effects)",
            ]
        )
        initializers = [
            f"Base(test, master, registers[{index}], base_address, backdoor, user_effects)"
        ]
        initializers.extend(
            f"{field_member_name}(*this, register_{index}_fields[{field_index}])"
            for field_index, field_member_name in enumerate(
                field_member_cpp_names[index]
            )
        )
        lines.append("        : " + ",\n          ".join(initializers) + " {}")
        lines.extend(
            [
                "",
                "    template <typename Function>",
                "    void for_each_field(Function&& function) {",
            ]
        )
        lines.extend(
            f"        function({field_member_name});"
            for field_member_name in field_member_cpp_names[index]
        )
        lines.extend(
            [
                "    }",
                "",
                "    template <typename Function>",
                "    void for_each_field(Function&& function) const {",
            ]
        )
        lines.extend(
            f"        function({field_member_name});"
            for field_member_name in field_member_cpp_names[index]
        )
        lines.append("    }")
        lines.extend(["};", ""])

    for node in view_nodes:
        lines.extend(
            [
                "template <cpptb::vc::MemoryMappedMaster Master>",
                "    requires std::unsigned_integral<typename Master::data_type>",
                f"struct {_view_type_name(node.path)} {{",
            ]
        )
        for member_name, member_type, _ in _view_member_specs(node, block):
            lines.append(f"    {member_type} {member_name};")
        lines.extend(["};", ""])

    lines.extend(
        [
            "template <cpptb::vc::MemoryMappedMaster Master>",
            "    requires std::unsigned_integral<typename Master::data_type>",
            f"class {class_name} {{",
            "   public:",
        ]
    )
    for index, register in enumerate(block.registers):
        lines.append(f"    Register{index}Handle<Master> {register.member};")
    for memory in block.memories:
        memory_type = (
            f"cpptb::vc::WideRegisterMemoryHandle<{memory.width}, Master>"
            if memory.width > 64
            else "cpptb::vc::RegisterMemoryHandle<Master>"
        )
        lines.append(f"    {memory_type} {memory.member};")
    for member_name, member_type, _ in root_view_specs:
        lines.append(f"    {member_type} {member_name};")
    lines.extend(
        [
            "",
            f"    {class_name}(cpptb::TestContext test, Master& master,",
            "             uint64_t base_address = 0,",
            "             cpptb::vc::RegisterBackdoor<uint64_t>* backdoor = nullptr,",
            "             cpptb::vc::RegisterMemoryBackdoor<",
            "                 typename Master::data_type>* memory_backdoor = nullptr,",
            "             cpptb::vc::WideRegisterBackdoor* wide_backdoor = nullptr,",
            "             cpptb::vc::WideRegisterMemoryBackdoor*",
            "                 wide_memory_backdoor = nullptr,",
            "             cpptb::vc::RegisterUserEffectPolicy*",
            "                 user_effects = nullptr)",
        ]
    )
    if block.registers or block.memories:
        initializers = [
            f"{register.member}(test, master, base_address, "
            + ("wide_backdoor, user_effects)" if register.width > 64 else "backdoor, user_effects)")
            for register in block.registers
        ]
        initializers.extend(
            f"{memory.member}(master, memories[{index}], base_address, "
            + (
                "wide_memory_backdoor)"
                if memory.width > 64
                else "memory_backdoor)"
            )
            for index, memory in enumerate(block.memories)
        )
        if block.registers and not has_wide_registers:
            handle_pointers = ", ".join(
                f"&{register.member}" for register in block.registers
            )
        initializers.extend(
            f"{member_name}({expression})"
            for member_name, _, expression in root_view_specs
        )
        if block.registers and not has_wide_registers:
            initializers.append(f"register_handles_{{{{{handle_pointers}}}}}")
        lines.append("        : " + ",\n          ".join(initializers) + " {}")
    else:
        lines.append("        {}")
    lines.extend(
        [
            "",
            f"    {class_name}(cpptb::TestContext test, Master& master,",
            "             cpptb::vc::RegisterUserEffectPolicy& user_effects,",
            "             uint64_t base_address = 0)",
            f"        : {class_name}(std::move(test), master, base_address,",
            "                      nullptr, nullptr, nullptr, nullptr,",
            "                      &user_effects) {}",
            "",
            "    template <typename Backdoor>",
            "        requires std::derived_from<",
            "                     Backdoor, cpptb::vc::RegisterBackdoor<uint64_t>> &&",
            "                 std::derived_from<",
            "                     Backdoor, cpptb::vc::RegisterMemoryBackdoor<",
            "                                   typename Master::data_type>>",
        ]
    )
    if has_wide_registers:
        lines.extend(
            [
                "              && std::derived_from<",
                "                     Backdoor, cpptb::vc::WideRegisterBackdoor>",
            ]
        )
    if has_wide_memories:
        lines.extend(
            [
                "              && std::derived_from<",
                "                     Backdoor, cpptb::vc::WideRegisterMemoryBackdoor>",
            ]
        )
    lines.extend(
        [
            f"    {class_name}(cpptb::TestContext test, Master& master,",
            "             uint64_t base_address, Backdoor* backdoor,",
            "             cpptb::vc::RegisterUserEffectPolicy*",
            "                 user_effects = nullptr)",
            f"        : {class_name}(std::move(test), master, base_address,",
            "                      static_cast<cpptb::vc::RegisterBackdoor<",
            "                          uint64_t>*>(backdoor),",
            "                      static_cast<cpptb::vc::RegisterMemoryBackdoor<",
            "                          typename Master::data_type>*>(backdoor),",
        ]
    )
    if has_wide_registers:
        lines.extend(
            [
                "                      static_cast<cpptb::vc::WideRegisterBackdoor*>(",
                "                          backdoor),",
            ]
        )
    else:
        lines.append("                      nullptr,")
    if has_wide_memories:
        lines.extend(
            [
                "                      static_cast<cpptb::vc::WideRegisterMemoryBackdoor*>(",
                "                          backdoor),",
                "                      user_effects) {}",
            ]
        )
    else:
        lines.extend(["                      nullptr,", "                      user_effects) {}"])
    lines.append("")
    lines.extend(
        [
            "    [[nodiscard]] static constexpr",
            "    const cpptb::vc::RegisterBlockDescriptor& descriptor() noexcept {",
            f"        return ::{namespace}::descriptor;",
            "    }",
            "",
            "    template <typename Function>",
            "    void for_each_memory(Function&& function) {",
        ]
    )
    lines.extend(
        f"        function({memory.member});" for memory in block.memories
    )
    lines.extend(
        [
            "    }",
            "",
            "    template <typename Function>",
            "    void for_each_memory(Function&& function) const {",
        ]
    )
    lines.extend(
        f"        function({memory.member});" for memory in block.memories
    )
    lines.extend(["    }", ""])
    lines.extend(["    void reset_all() {"])
    lines.extend(
        f"        {register.member}.reset();" for register in block.registers
    )
    lines.extend(
        [
            "    }",
            "",
            "    void set_auto_predict(bool enabled) {",
        ]
    )
    lines.extend(
        f"        {register.member}.set_auto_predict(enabled);"
        for register in block.registers
    )
    lines.extend(
        [
            "    }",
            "",
            "    cpptb::coro::Task<void> update_all() {",
        ]
    )
    lines.extend(
        f"        static_cast<void>(co_await {register.member}.update());"
        for register in block.registers
    )
    lines.extend(
        [
            "        co_return;",
            "    }",
            "",
            "    cpptb::coro::Task<void> mirror_all(",
            "        cpptb::vc::MirrorCheck check = "
            "cpptb::vc::MirrorCheck::Enabled) {",
        ]
    )
    for register in block.registers:
        readable = not register.fields or any(
            field.access
            in {
                "cpptb::vc::RegisterAccess::ReadOnly",
                "cpptb::vc::RegisterAccess::ReadWrite",
                "cpptb::vc::RegisterAccess::ReadWriteOnce",
            }
            for field in register.fields
        )
        if readable:
            lines.append(
                f"        static_cast<void>(co_await {register.member}.mirror(check));"
            )
    lines.extend(["        co_return;", "    }", ""])
    lines.extend(
        [
            "    template <typename Function>",
            "    cpptb::coro::Task<void> for_each_register_async(",
            "        Function& function) {",
        ]
    )
    lines.extend(
        f"        co_await function({register.member});"
        for register in block.registers
    )
    lines.extend(["        co_return;", "    }", ""])
    if not has_wide_registers:
        lines.extend(
            [
                "    std::span<cpptb::vc::RegisterHandle<Master>* const>",
                "    register_handles() noexcept {",
                "        return register_handles_;",
                "    }",
                "",
                "    std::span<cpptb::vc::RegisterHandle<Master>* const>",
                "    register_handles() const noexcept {",
                "        return register_handles_;",
                "    }",
                "",
                "    template <typename Function>",
                "    void for_each_register(Function&& function) {",
                "        for (auto* handle : register_handles_) function(*handle);",
                "    }",
                "",
                "    template <typename Function>",
                "    void for_each_register(Function&& function) const {",
                "        for (const auto* handle : register_handles_) function(*handle);",
                "    }",
                "",
                "   private:",
                f"    std::array<cpptb::vc::RegisterHandle<Master>*, {len(block.registers)}>",
                "        register_handles_{};",
            ]
        )
    else:
        lines.extend(
            [
                "    template <typename Function>",
                "    void for_each_register(Function&& function) {",
            ]
        )
        lines.extend(
            f"        function({register.member});" for register in block.registers
        )
        lines.extend(
            [
                "    }",
                "",
                "    template <typename Function>",
                "    void for_each_register(Function&& function) const {",
            ]
        )
        lines.extend(
            f"        function({register.member});" for register in block.registers
        )
        lines.extend(["    }", "", "   private:"])
    lines.extend(
        [
            "};",
            "",
            f"}}  // namespace {namespace}",
            "",
        ]
    )
    return "\n".join(lines)


def export_register_model(
    top_node: object,
    output: str | Path,
    *,
    namespace: str | None = None,
    class_name: str = "RegModel",
    endianness: str = "little",
) -> Path:
    block = extract_register_block(top_node, endianness=endianness)
    selected_namespace = cpp_identifier(namespace or block.name)
    selected_class = cpp_identifier(class_name)
    path = Path(output)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        render_cpp(block, selected_namespace, selected_class), encoding="utf-8"
    )
    return path
