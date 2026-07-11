"""Simulator-independent elaborated design model used by code generation."""

from __future__ import annotations

from dataclasses import dataclass


class CodegenError(RuntimeError):
    pass


@dataclass(frozen=True)
class Port:
    name: str
    direction: str
    width: int
    cpp_path: tuple[str, ...] = ()
    type_kind: str = "integral"
    signed: bool = False
    four_state: bool = True


@dataclass(frozen=True)
class DesignIR:
    module: str
    ports: tuple[Port, ...]

    def transport_signature(self) -> tuple[tuple[str, str, int], ...]:
        """Return the frontend-independent contract consumed by today's transport."""
        return tuple((port.name, port.direction, port.width) for port in self.ports)
