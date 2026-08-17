"""Generate cpptb register models from native RgGen metadata."""

from __future__ import annotations

import argparse
import itertools
import json
import math
import sys
import tomllib
from dataclasses import replace
from pathlib import Path
from typing import Any, Iterable, Sequence

from cpptb_peakrdl.codegen import (
    BackdoorSlice,
    Field,
    Memory,
    Register,
    RegisterBlock,
    _member_names,
    cpp_identifier,
    render_cpp,
)


class RgGenCodegenError(ValueError):
    """An RgGen contract cannot be represented by the cpptb model."""


_ACCESS = {
    "none": "cpptb::vc::RegisterAccess::None",
    "ro": "cpptb::vc::RegisterAccess::ReadOnly",
    "wo": "cpptb::vc::RegisterAccess::WriteOnly",
    "rw": "cpptb::vc::RegisterAccess::ReadWrite",
}

_FIELD_TYPES = {
    "rw": ("rw", "", "", False),
    "rwtrg": ("rw", "", "", False),
    "ro": ("ro", "", "", True),
    "rotrg": ("ro", "", "", True),
    "rof": ("ro", "", "", False),
    "rohw": ("ro", "", "", True),
    "wo": ("wo", "", "", False),
    "wotrg": ("wo", "", "", False),
    "w1trg": ("wo", "", "", False),
    "w0trg": ("wo", "", "", False),
    "wrc": ("rw", "rclr", "", False),
    "wrs": ("rw", "rset", "", False),
    "w0s": ("rw", "", "wzs", False),
    "w1s": ("rw", "", "woset", False),
    "ws": ("rw", "", "wset", False),
    "wos": ("wo", "", "wset", False),
    "w0c": ("rw", "", "wzc", False),
    "w1c": ("rw", "", "woclr", False),
    "wc": ("rw", "", "wclr", False),
    "woc": ("wo", "", "wclr", False),
    "w0t": ("rw", "", "wzt", False),
    "w1t": ("rw", "", "wot", False),
    "rwc": ("rw", "", "", True),
    "rws": ("rw", "", "", True),
    "rwe": ("rw", "", "", True),
}

_READ_EFFECT = {
    "": "cpptb::vc::RegisterReadEffect::None",
    "rclr": "cpptb::vc::RegisterReadEffect::Clear",
    "rset": "cpptb::vc::RegisterReadEffect::Set",
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
}


def _fail(path: str, message: str) -> None:
    raise RgGenCodegenError(f"{path}: {message}")


def _integer(value: Any, path: str, attribute: str) -> int:
    if isinstance(value, dict) and "default" in value:
        value = value["default"]
    if isinstance(value, bool):
        _fail(path, f"RgGen {attribute} must be an integer")
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        try:
            return int(value.replace("_", ""), 0)
        except ValueError:
            pass
    _fail(path, f"unsupported RgGen {attribute} value {value!r}")


def _structure(value: Any, path: str) -> dict[str, Any]:
    if isinstance(value, dict):
        return dict(value)
    if not isinstance(value, list):
        _fail(path, "RgGen structure must be a mapping or sequence")
    result: dict[str, Any] = {}
    for item in value:
        if not isinstance(item, dict):
            _fail(path, "RgGen structure sequence contains a non-mapping item")
        for key, child in item.items():
            if key in {
                "register",
                "registers",
                "register_file",
                "register_files",
                "bit_field",
                "bit_fields",
            }:
                plural = key if key.endswith("s") else f"{key}s"
                values = (
                    child
                    if key.endswith("s") and isinstance(child, list)
                    else [child]
                )
                result.setdefault(plural, []).extend(values)
            else:
                result[key] = child
    return result


def _children(node: dict[str, Any], singular: str, path: str) -> list[dict[str, Any]]:
    values: list[Any] = []
    if singular in node:
        values.append(node[singular])
    plural = f"{singular}s"
    if plural in node:
        selected = node[plural]
        values.extend(selected if isinstance(selected, list) else [selected])
    return [
        _structure(value, f"{path}.{singular}[{index}]")
        for index, value in enumerate(values)
    ]


def _root_blocks(document: Any) -> list[dict[str, Any]]:
    if isinstance(document, dict):
        return _children(document, "register_block", "<root>")
    if isinstance(document, list):
        blocks: list[dict[str, Any]] = []
        for index, item in enumerate(document):
            if not isinstance(item, dict):
                _fail("<root>", f"item {index} is not a mapping")
            blocks.extend(_children(item, "register_block", "<root>"))
        return blocks
    _fail("<root>", "RgGen document must contain register_block metadata")


def _load(path: Path) -> Any:
    suffix = path.suffix.lower()
    try:
        if suffix in {".yaml", ".yml"}:
            import yaml

            try:
                return yaml.safe_load(path.read_text(encoding="utf-8"))
            except (OSError, yaml.YAMLError) as error:
                raise RgGenCodegenError(
                    f"{path}: cannot load RgGen metadata: {error}"
                ) from error
        if suffix == ".json":
            return json.loads(path.read_text(encoding="utf-8"))
        if suffix == ".toml":
            with path.open("rb") as stream:
                return tomllib.load(stream)
    except (OSError, ValueError) as error:
        raise RgGenCodegenError(f"{path}: cannot load RgGen metadata: {error}") from error
    raise RgGenCodegenError(
        f"{path}: unsupported RgGen format; use .yaml, .yml, .json, or .toml"
    )


def _array_spec(value: Any, path: str) -> tuple[tuple[int, ...], int | None]:
    if value is None:
        return (1,), None
    values = value if isinstance(value, list) else [value]
    dimensions: list[int] = []
    step: int | None = None
    for item in values:
        if isinstance(item, dict) and "step" in item:
            if set(item) != {"step"} or step is not None:
                _fail(path, "RgGen size has malformed or duplicate step metadata")
            step = _integer(item["step"], path, "size.step")
        else:
            dimensions.append(_integer(item, path, "size"))
    if not dimensions or any(size <= 0 for size in dimensions):
        _fail(path, "RgGen size dimensions must be greater than zero")
    if step is not None and step <= 0:
        _fail(path, "RgGen size step must be greater than zero")
    return tuple(dimensions), step


def _dimensions(value: Any, path: str) -> tuple[int, ...]:
    return _array_spec(value, path)[0]


def _array_indices(dimensions: tuple[int, ...]) -> Iterable[tuple[int, ...]]:
    return itertools.product(*(range(size) for size in dimensions))


def _indexed_name(name: str, indices: tuple[int, ...], dimensions: tuple[int, ...]) -> str:
    if math.prod(dimensions) == 1:
        return name
    return name + "".join(f"[{index}]" for index in indices)


def _register_type(value: Any, path: str) -> str:
    selected = value[0] if isinstance(value, list) and value else value
    name = str(selected or "default")
    if name == "indirect":
        _fail(path, "RgGen indirect register selection is not yet representable")
    if name not in {"default", "rw", "external"}:
        _fail(path, f"unsupported RgGen register type {name!r}")
    return name


def _field_type(value: Any, path: str) -> tuple[str, str, str, bool]:
    selected = value[0] if isinstance(value, list) and value else value
    name = str(selected or "rw")
    try:
        return _FIELD_TYPES[name]
    except KeyError as error:
        _fail(path, f"unsupported RgGen bit field type {name!r}")
        raise AssertionError from error


def _reset_value(value: Any, path: str, index: int) -> tuple[int, bool]:
    if value is None:
        return 0, False
    if isinstance(value, list):
        if index >= len(value):
            _fail(path, "RgGen initial_value array is shorter than the field sequence")
        value = value[index]
    return _integer(value, path, "initial_value"), True


def _extract_fields(
    node: dict[str, Any], register_path: str, register_name: str, width: int
) -> tuple[tuple[Field, ...], int, int, tuple[BackdoorSlice, ...], bool]:
    fields: list[Field] = []
    reset_value = 0
    reset_mask = 0
    register_hdl_path = node.get("cpptb_hdl_path")
    register_slices: list[BackdoorSlice] = []
    next_lsb = 0
    occupied_mask = 0
    for raw_field in _children(node, "bit_field", register_path):
        assignment = raw_field.get("bit_assignment")
        if not isinstance(assignment, dict):
            _fail(register_path, "RgGen bit field is missing bit_assignment")
        lsb = _integer(assignment.get("lsb", next_lsb), register_path, "bit_assignment.lsb")
        field_width = _integer(assignment.get("width", 1), register_path, "bit_assignment.width")
        sequence_size = _integer(
            assignment.get("sequence_size", 1), register_path, "bit_assignment.sequence_size"
        )
        step = _integer(assignment.get("step", field_width), register_path, "bit_assignment.step")
        if field_width <= 0 or sequence_size <= 0 or step <= 0:
            _fail(register_path, "RgGen bit assignment values must be greater than zero")
        base_name = str(raw_field.get("name") or register_name)
        access, read_effect, write_effect, volatile = _field_type(
            raw_field.get("type"), f"{register_path}.{base_name}"
        )
        for sequence_index in range(sequence_size):
            selected_lsb = lsb + sequence_index * step
            field_name = (
                base_name if sequence_size == 1 else f"{base_name}[{sequence_index}]"
            )
            field_path = f"{register_path}.{field_name}"
            if selected_lsb < 0 or selected_lsb + field_width > width:
                _fail(field_path, "RgGen bit assignment exceeds the register width")
            initial, has_reset = _reset_value(
                raw_field.get("initial_value"), field_path, sequence_index
            )
            mask = (1 << field_width) - 1
            positioned_mask = mask << selected_lsb
            if occupied_mask & positioned_mask:
                _fail(field_path, "RgGen bit assignment overlaps another bit field")
            occupied_mask |= positioned_mask
            if initial & ~mask:
                _fail(field_path, "RgGen initial_value does not fit the bit field")
            reset_value |= initial << selected_lsb
            if has_reset:
                reset_mask |= mask << selected_lsb
            field_hdl_path = raw_field.get("cpptb_hdl_path")
            slices: tuple[BackdoorSlice, ...] = ()
            if register_hdl_path:
                slices = (
                    BackdoorSlice(str(register_hdl_path), selected_lsb, field_width),
                )
            elif field_hdl_path:
                slices = (
                    BackdoorSlice(str(field_hdl_path), selected_lsb, field_width),
                )
                register_slices.extend(slices)
            fields.append(
                Field(
                    name=field_name,
                    path=field_path,
                    lsb=selected_lsb,
                    width=field_width,
                    access=_ACCESS[access],
                    read_effect=_READ_EFFECT[read_effect],
                    write_effect=_WRITE_EFFECT[write_effect],
                    reset_value=initial,
                    reset_mask=mask if has_reset else 0,
                    volatile=volatile,
                    backdoor_slices=slices,
                )
            )
        next_lsb = max(next_lsb, lsb + sequence_size * step)
    if register_hdl_path:
        register_slices = [BackdoorSlice(str(register_hdl_path), 0, width)]
    complete = bool(register_hdl_path) or (
        bool(fields) and all(field.backdoor_slices for field in fields)
    )
    return tuple(fields), reset_value, reset_mask, tuple(register_slices), complete


def extract_rggen_block(document: Any, *, block_name: str | None = None) -> RegisterBlock:
    blocks = _root_blocks(document)
    if block_name is not None:
        blocks = [block for block in blocks if str(block.get("name")) == block_name]
    if len(blocks) != 1:
        names = ", ".join(str(block.get("name", "<unnamed>")) for block in blocks)
        suffix = f"; available blocks: {names}" if names else ""
        raise RgGenCodegenError(
            f"<root>: select exactly one RgGen register block with --block{suffix}"
        )
    root = blocks[0]
    name = str(root.get("name") or "")
    if not name:
        _fail("<root>", "RgGen register block has no name")
    bus_width = _integer(root.get("bus_width", 32), name, "bus_width")
    if bus_width <= 0 or bus_width % 8 != 0:
        _fail(name, "RgGen bus_width must be a positive multiple of eight")
    register_bytes = bus_width // 8
    registers: list[Register] = []
    memories: list[Memory] = []

    def walk(container: dict[str, Any], path: str, base_address: int) -> int:
        cursor = 0
        for raw in _children(container, "register", path):
            raw_name = str(raw.get("name") or "")
            register_path = f"{path}.{raw_name or '<unnamed>'}"
            if not raw_name:
                _fail(register_path, "RgGen register has no name")
            register_type = _register_type(raw.get("type"), register_path)
            offset = _integer(raw.get("offset_address", cursor), register_path, "offset_address")
            dimensions, size_step = _array_spec(
                raw.get("size"), register_path
            )
            count = math.prod(dimensions)
            step_value = raw.get(
                "step", size_step if size_step is not None else register_bytes
            )
            step = _integer(step_value, register_path, "step")
            if offset < 0 or step <= 0:
                _fail(
                    register_path,
                    "RgGen register offset must be non-negative and step must be positive",
                )
            raw_fields = _children(raw, "bit_field", register_path)
            if register_type == "external" and not raw_fields and count > 1:
                memories.append(
                    Memory(
                        name=raw_name,
                        member="",
                        path=register_path,
                        address=base_address + offset,
                        entries=count,
                        width=bus_width,
                        access_width=bus_width,
                        access=_ACCESS["rw"],
                        backdoor_path=(
                            str(raw["cpptb_hdl_path"])
                            if raw.get("cpptb_hdl_path")
                            else None
                        ),
                    )
                )
                cursor = max(cursor, offset + count * step)
                continue
            for linear_index, indices in enumerate(_array_indices(dimensions)):
                selected_name = _indexed_name(raw_name, indices, dimensions)
                selected_path = f"{path}.{selected_name}"
                fields, reset_value, reset_mask, slices, complete = _extract_fields(
                    raw, selected_path, selected_name, bus_width
                )
                registers.append(
                    Register(
                        name=selected_name,
                        member="",
                        path=selected_path,
                        address=base_address + offset + linear_index * step,
                        width=bus_width,
                        access_width=bus_width,
                        endianness="cpptb::vc::RegisterEndianness::Little",
                        reset_value=reset_value,
                        reset_mask=reset_mask,
                        fields=fields,
                        backdoor_slices=slices,
                        backdoor_complete=complete,
                    )
                )
            cursor = max(cursor, offset + count * step)

        for raw_file in _children(container, "register_file", path):
            file_name = str(raw_file.get("name") or "")
            file_path = f"{path}.{file_name or '<unnamed>'}"
            if not file_name:
                _fail(file_path, "RgGen register file has no name")
            offset = _integer(raw_file.get("offset_address", cursor), file_path, "offset_address")
            dimensions, size_step = _array_spec(
                raw_file.get("size"), file_path
            )
            indices_list = list(_array_indices(dimensions))
            first_name = _indexed_name(file_name, indices_list[0], dimensions)
            first_span = walk(
                raw_file,
                f"{path}.{first_name}",
                base_address + offset,
            )
            explicit_step = raw_file.get("step", size_step)
            if explicit_step is None:
                stride = first_span
            else:
                stride = _integer(explicit_step, file_path, "step")
            if stride <= 0:
                _fail(file_path, "RgGen arrayed register file has no address span")
            if stride < first_span:
                _fail(file_path, "RgGen register file step overlaps an array element")
            for linear_index, indices in enumerate(indices_list[1:], start=1):
                selected_name = _indexed_name(file_name, indices, dimensions)
                walk(
                    raw_file,
                    f"{path}.{selected_name}",
                    base_address + offset + linear_index * stride,
                )
            cursor = max(
                cursor,
                offset + (len(indices_list) - 1) * stride + first_span,
            )
        return cursor

    walk(root, name, 0)
    registers.sort(key=lambda register: (register.address, register.path))
    memories.sort(key=lambda memory: (memory.address, memory.path))
    register_members = _member_names(
        [register.path for register in registers],
        reserved={
            "descriptor",
            "for_each_memory",
            "for_each_register",
            "for_each_register_async",
            "mirror_all",
            "register_handles",
            "reset_all",
            "update_all",
        },
    )
    registers = [
        replace(register, member=member)
        for register, member in zip(registers, register_members, strict=True)
    ]
    used = set(register_members)
    selected_memories: list[Memory] = []
    for memory in memories:
        member = _member_names([memory.path])[0]
        base = member
        suffix = 2
        while member in used:
            member = f"{base}_{suffix}"
            suffix += 1
        used.add(member)
        selected_memories.append(replace(memory, member=member))
    return RegisterBlock(name, tuple(registers), tuple(selected_memories))


def export_rggen_model(
    input_path: str | Path,
    output: str | Path,
    *,
    block_name: str | None = None,
    namespace: str | None = None,
    class_name: str = "RegModel",
) -> Path:
    source = Path(input_path)
    block = extract_rggen_block(_load(source), block_name=block_name)
    target = Path(output)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(
        render_cpp(
            block,
            cpp_identifier(namespace or block.name),
            cpp_identifier(class_name),
        ),
        encoding="utf-8",
    )
    return target


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="cpptb-rggen",
        description="Generate a typed cpptb register model from RgGen metadata.",
    )
    parser.add_argument("input", type=Path, help="RgGen YAML, JSON, or TOML file")
    parser.add_argument("-o", "--output", type=Path, required=True)
    parser.add_argument("--block", help="select one register block by name")
    parser.add_argument("--namespace", help="generated C++ namespace")
    parser.add_argument(
        "--class-name", default="RegModel", help="generated C++ model class"
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        path = export_rggen_model(
            args.input,
            args.output,
            block_name=args.block,
            namespace=args.namespace,
            class_name=args.class_name,
        )
    except (OSError, RgGenCodegenError) as error:
        print(f"cpptb-rggen: {error}", file=sys.stderr)
        return 2
    print(f"generated {path.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
