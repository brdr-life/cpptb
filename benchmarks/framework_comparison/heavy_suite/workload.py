"""Shared stimulus and expected-result contract for the heavy benchmarks."""

from __future__ import annotations

from dataclasses import dataclass
from functools import lru_cache


MASK32 = 0xFFFF_FFFF
WORKLOADS = ("streaming_fir", "packet_crc32", "matrix4x4")
MODES = ("pure_sv", "cpp_dpi", "cpp_vpi", "cocotb")
DEFAULT_ITERATIONS = {
    "streaming_fir": 100_000,
    "packet_crc32": 2_000,
    "matrix4x4": 2_000,
}


@dataclass(frozen=True)
class WorkloadDescription:
    title: str
    purpose: str
    unit: str


DESCRIPTIONS = {
    "streaming_fir": WorkloadDescription(
        "32-tap streaming FIR",
        "signed fixed-width stimulus, 32 MACs per sample, history model, and streaming scoreboard",
        "samples",
    ),
    "packet_crc32": WorkloadDescription(
        "Variable-length packet CRC32",
        "32-95 byte frames, byte-wise reflected CRC32 reference model, framing, and result checks",
        "packets",
    ),
    "matrix4x4": WorkloadDescription(
        "4x4 signed matrix accelerator",
        "block loading, 64 software MACs per block, 16 indexed outputs, and full result scoreboard",
        "matrix blocks",
    ),
}


def stimulus(ordinal: int) -> int:
    return ((((ordinal + 1) * 0x1F12_3BB5) & MASK32) ^ 0xC001_D00D) & MASK32


def signed16(value: int) -> int:
    value &= 0xFFFF
    return value - 0x1_0000 if value & 0x8000 else value


def fir_coefficient(tap: int) -> int:
    return ((tap * 7) % 19) - 9


def crc32_byte(current: int, data: int) -> int:
    value = current ^ data
    for _ in range(8):
        value = ((value >> 1) ^ 0xEDB8_8320) if value & 1 else value >> 1
    return value & MASK32


def matrix_value(block: int, matrix: int, index: int) -> int:
    return ((stimulus(block * 32 + matrix * 16 + index) >> 8) & 0x7FF) - 1024


def fold(checksum: int, value: int) -> int:
    return ((checksum ^ (value & MASK32)) * 0x0100_0193) & MASK32


@lru_cache(maxsize=None)
def expected_result(workload: str, iterations: int) -> dict[str, int]:
    if workload not in WORKLOADS:
        raise ValueError(f"unknown heavy workload: {workload}")
    if iterations <= 0:
        raise ValueError("iterations must be positive")

    checksum = 0x811C_9DC5
    if workload == "streaming_fir":
        history = [0] * 32
        for iteration in range(iterations):
            sample = signed16(stimulus(iteration))
            result = sample * fir_coefficient(0)
            result += sum(
                history[tap - 1] * fir_coefficient(tap)
                for tap in range(1, 32)
            )
            history[1:] = history[:-1]
            history[0] = sample
            checksum = fold(checksum, result)
        checks = iterations + 1
    elif workload == "packet_crc32":
        for packet in range(iterations):
            crc = 0xFFFF_FFFF
            for offset in range(32 + (packet & 63)):
                crc = crc32_byte(crc, stimulus(packet * 96 + offset) & 0xFF)
            checksum = fold(checksum, ~crc)
        checks = iterations + 1
    else:
        for block in range(iterations):
            matrix_a = [matrix_value(block, 0, index) for index in range(16)]
            matrix_b = [matrix_value(block, 1, index) for index in range(16)]
            for output in range(16):
                row, column = divmod(output, 4)
                result = sum(
                    matrix_a[row * 4 + element]
                    * matrix_b[element * 4 + column]
                    for element in range(4)
                )
                checksum = fold(checksum, result)
        checks = iterations * 32 + 1

    return {
        "iterations": iterations,
        "transactions": iterations,
        "checks": checks,
        "checksum": checksum,
        "failures": 0,
    }


def rotated_mode_order(round_index: int) -> tuple[str, ...]:
    offset = round_index % len(MODES)
    return MODES[offset:] + MODES[:offset]
