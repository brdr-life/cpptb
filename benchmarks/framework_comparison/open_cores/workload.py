"""Shared stimulus and expected-result contract for open-core benchmarks."""

from __future__ import annotations

from dataclasses import dataclass
from functools import lru_cache


MASK32 = 0xFFFF_FFFF
WORKLOADS = ("picorv32_firmware", "secworks_aes128", "ethernet_fcs64")
MODES = ("pure_sv", "cpp_dpi", "cpp_vpi", "cocotb")
DEFAULT_ITERATIONS = {
    "picorv32_firmware": 20_000,
    "secworks_aes128": 4_000,
    "ethernet_fcs64": 2_000,
}


@dataclass(frozen=True)
class WorkloadDescription:
    title: str
    purpose: str
    unit: str
    upstream: str


DESCRIPTIONS = {
    "picorv32_firmware": WorkloadDescription(
        "PicoRV32 firmware kernel",
        "boots RV32I firmware and runs a dependency-heavy xorshift loop",
        "firmware loop iterations",
        "YosysHQ/picorv32",
    ),
    "secworks_aes128": WorkloadDescription(
        "secworks AES-128",
        "programs the register interface and checks iterative block encryption",
        "AES blocks",
        "secworks/aes",
    ),
    "ethernet_fcs64": WorkloadDescription(
        "64-bit Ethernet FCS",
        "streams variable Ethernet frames with partial beats and valid gaps",
        "Ethernet frames",
        "alexforencich/verilog-ethernet",
    ),
}


AES_CIPHERTEXT_WORDS = (
    (0x3AD7_7BB4, 0x0D7A_3660, 0xA89E_CAF3, 0x2466_EF97),
    (0xF5D3_D585, 0x03B9_699D, 0xE785_895A, 0x96FD_BAAF),
    (0x43B1_CD7F, 0x598E_CE23, 0x881B_00E3, 0xED03_0688),
    (0x7B0C_785E, 0x27E8_AD3F, 0x8223_2071, 0x0472_5DD4),
)


def fold(checksum: int, value: int) -> int:
    return ((checksum ^ (value & MASK32)) * 0x0100_0193) & MASK32


def xorshift32(value: int) -> int:
    value ^= (value << 13) & MASK32
    value ^= value >> 17
    value ^= (value << 5) & MASK32
    return value & MASK32


def stimulus(ordinal: int) -> int:
    return ((((ordinal + 1) * 0x1F12_3BB5) & MASK32) ^ 0xC001_D00D) & MASK32


def frame_length(packet: int) -> int:
    return 64 + ((packet * 37) % 1455)


def frame_byte(packet: int, offset: int) -> int:
    return stimulus(packet * 2048 + offset) & 0xFF


def crc32_byte(current: int, data: int) -> int:
    value = current ^ data
    for _ in range(8):
        value = ((value >> 1) ^ 0xEDB8_8320) if value & 1 else value >> 1
    return value & MASK32


@lru_cache(maxsize=None)
def expected_result(workload: str, iterations: int) -> dict[str, int]:
    if workload not in WORKLOADS:
        raise ValueError(f"unknown open-core workload: {workload}")
    if iterations <= 0:
        raise ValueError("iterations must be positive")

    checksum = 0x811C_9DC5
    if workload == "picorv32_firmware":
        result = 0x1234_5678
        for _ in range(iterations):
            result = xorshift32(result)
        checksum = fold(checksum, result)
        checks = 2
    elif workload == "secworks_aes128":
        for block in range(iterations):
            for word in AES_CIPHERTEXT_WORDS[block & 3]:
                checksum = fold(checksum, word)
        checks = iterations * 4 + 1
    else:
        for packet in range(iterations):
            crc = 0xFFFF_FFFF
            for offset in range(frame_length(packet)):
                crc = crc32_byte(crc, frame_byte(packet, offset))
            checksum = fold(checksum, ~crc)
        checks = iterations

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
