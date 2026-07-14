"""Simulator-independent elaborated design model used by code generation."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import TypeAlias


class CodegenError(RuntimeError):
    pass


@dataclass(frozen=True)
class UnpackedRange:
    left: int
    right: int

    @property
    def low(self) -> int:
        return min(self.left, self.right)

    @property
    def high(self) -> int:
        return max(self.left, self.right)

    @property
    def size(self) -> int:
        return self.high - self.low + 1


@dataclass(frozen=True)
class PackedRange:
    """One declared packed dimension, preserving its SystemVerilog direction."""

    left: int
    right: int

    @property
    def width(self) -> int:
        return abs(self.left - self.right) + 1


@dataclass(frozen=True)
class PackedIntegralType:
    width: int
    signed: bool
    four_state: bool
    ranges: tuple[PackedRange, ...] = ()
    declared_name: str | None = field(default=None, compare=False)

    def structural_signature(self) -> tuple:
        return (
            "integral",
            self.width,
            self.signed,
            self.four_state,
            tuple((item.left, item.right) for item in self.ranges),
        )


@dataclass(frozen=True)
class PackedEnumValue:
    name: str
    value: int


@dataclass(frozen=True)
class PackedEnumType:
    base: PackedIntegralType
    values: tuple[PackedEnumValue, ...]
    declared_name: str | None = field(default=None, compare=False)

    @property
    def width(self) -> int:
        return self.base.width

    @property
    def signed(self) -> bool:
        return self.base.signed

    @property
    def four_state(self) -> bool:
        return self.base.four_state

    def structural_signature(self) -> tuple:
        return (
            "enum",
            self.base.structural_signature(),
            tuple((item.name, item.value) for item in self.values),
        )


@dataclass(frozen=True)
class PackedField:
    name: str
    data_type: PackedType
    bit_offset: int

    @property
    def width(self) -> int:
        return self.data_type.width

    def structural_signature(self) -> tuple:
        return (
            self.name,
            self.bit_offset,
            self.data_type.structural_signature(),
        )


@dataclass(frozen=True)
class PackedStructType:
    width: int
    signed: bool
    four_state: bool
    fields: tuple[PackedField, ...]
    declared_name: str | None = field(default=None, compare=False)

    def structural_signature(self) -> tuple:
        return (
            "struct",
            self.width,
            self.signed,
            self.four_state,
            tuple(item.structural_signature() for item in self.fields),
        )


@dataclass(frozen=True)
class PackedUnionType:
    """Elaborated packed-union layout; transport generation remains unsupported."""

    width: int
    signed: bool
    four_state: bool
    fields: tuple[PackedField, ...]
    declared_name: str | None = field(default=None, compare=False)

    def structural_signature(self) -> tuple:
        return (
            "union",
            self.width,
            self.signed,
            self.four_state,
            tuple(item.structural_signature() for item in self.fields),
        )


PackedType: TypeAlias = (
    PackedIntegralType | PackedEnumType | PackedStructType | PackedUnionType
)


class TransportPortSignature(tuple):
    """Legacy six-item signature with recursive packed layout equality."""

    packed_type_signature: tuple | None
    transport: str

    def __new__(
        cls,
        values: tuple,
        packed_type_signature: tuple | None,
        transport: str = "packed",
    ) -> TransportPortSignature:
        result = super().__new__(cls, values)
        result.packed_type_signature = packed_type_signature
        result.transport = transport
        return result

    def __eq__(self, other: object) -> bool:
        if not tuple.__eq__(self, other):
            return False
        if isinstance(other, TransportPortSignature):
            return (
                self.packed_type_signature == other.packed_type_signature
                and self.transport == other.transport
            )
        return True

    __hash__ = tuple.__hash__


@dataclass(frozen=True)
class Port:
    name: str
    direction: str
    width: int
    cpp_path: tuple[str, ...] = ()
    type_kind: str = "integral"
    signed: bool = False
    four_state: bool = True
    unpacked: tuple[UnpackedRange, ...] = ()
    packed_type: PackedType | None = field(default=None, compare=False)
    transport: str = "packed"


@dataclass(frozen=True)
class Internal:
    hdl_path: str
    cpp_path: tuple[str, ...]
    symbol_kind: str
    width: int
    access: str = "read"
    forceable: bool = False
    type_kind: str = "integral"
    signed: bool = False
    four_state: bool = True
    unpacked: tuple[UnpackedRange, ...] = ()
    packed_type: PackedType | None = field(default=None, compare=False)

    @property
    def writable(self) -> bool:
        return self.access == "read_write"


@dataclass(frozen=True)
class HierarchyScope:
    """One elaborated module, interface, or generate scope below the DUT."""

    hdl_path: str
    cpp_path: tuple[str, ...]
    symbol_kind: str


@dataclass(frozen=True)
class HierarchySignal:
    """One elaborated variable or net available for on-demand access."""

    hdl_path: str
    cpp_path: tuple[str, ...]
    symbol_kind: str
    width: int
    type_kind: str = "integral"
    signed: bool = False
    four_state: bool = True
    unpacked: tuple[UnpackedRange, ...] = ()
    packed_type: PackedType | None = field(default=None, compare=False)

    @property
    def depositable(self) -> bool:
        return self.symbol_kind == "variable"


@dataclass(frozen=True)
class HierarchyParameter:
    """An elaborated integral parameter exposed as a C++ constant."""

    hdl_path: str
    cpp_path: tuple[str, ...]
    value: int
    local: bool = False


@dataclass(frozen=True)
class HierarchyCatalog:
    scopes: tuple[HierarchyScope, ...] = ()
    signals: tuple[HierarchySignal, ...] = ()
    parameters: tuple[HierarchyParameter, ...] = ()


@dataclass(frozen=True)
class DesignIR:
    module: str
    ports: tuple[Port, ...]
    internals: tuple[Internal, ...] = ()
    hierarchy: HierarchyCatalog = field(default_factory=HierarchyCatalog)

    def transport_signature(
        self,
    ) -> tuple[TransportPortSignature, ...]:
        """Return the frontend-independent typed transport contract."""
        return tuple(
            TransportPortSignature(
                (
                    port.name,
                    port.direction,
                    port.width,
                    port.signed,
                    port.four_state,
                    tuple(
                        (dimension.left, dimension.right)
                        for dimension in port.unpacked
                    ),
                ),
                (
                    port.packed_type.structural_signature()
                    if port.packed_type is not None
                    else None
                ),
                port.transport,
            )
            for port in self.ports
        )
