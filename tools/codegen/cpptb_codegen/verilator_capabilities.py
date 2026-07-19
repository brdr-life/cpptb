"""Semantic capability probes for experimental Verilator features."""

from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Sequence


_PROBE_SCHEMA = 2
_RESULT_PATTERN = re.compile(
    r"CPPTB_FOUR_STATE_PROBE "
    r"sv=(?P<sv>[01]) "
    r"net=(?P<net>[01]) "
    r"dpi_from_sv=(?P<dpi_from_sv>[01]) "
    r"dpi_round_trip=(?P<dpi_round_trip>[01])"
)

_SV_PROBE = r'''module cpptb_four_state_probe;
  timeunit 1ps;
  timeprecision 1ps;

  logic [7:0] source_value;
  tri [3:0] undriven;
  logic drive_zero;
  logic drive_one;
  tri resolved_bus;

  assign resolved_bus = drive_zero ? 1'b0 : 1'bz;
  assign resolved_bus = drive_one ? 1'b1 : 1'bz;

  import "DPI-C" context function void cpptb_run_four_state_probe(
      input logic [7:0] from_sv,
      input bit sv_preserves,
      input bit net_resolves);

  export "DPI-C" function cpptb_probe_set;
  function void cpptb_probe_set(input logic [7:0] value);
    source_value = value;
  endfunction

  export "DPI-C" function cpptb_probe_get;
  function void cpptb_probe_get(output logic [7:0] value);
    value = source_value;
  endfunction

  initial begin
    source_value = 8'b10xz0011;
    drive_zero = 1'b1;
    drive_one = 1'b1;
    #1ps;
    cpptb_run_four_state_probe(
        source_value,
        source_value === 8'b10xz0011,
        (undriven === 4'bzzzz) && (resolved_bus === 1'bx));
    $finish;
  end
endmodule
'''

_CPP_PROBE = r'''#include <cstdio>

#include "svdpi.h"

extern "C" void cpptb_probe_set(const svLogicVecVal* value);
extern "C" void cpptb_probe_get(svLogicVecVal* value);

extern "C" void cpptb_run_four_state_probe(
    const svLogicVecVal* from_sv, svBit sv_preserves, svBit net_resolves) {
    const bool dpi_from_sv =
        from_sv[0].aval == 0xa3u && from_sv[0].bval == 0x30u;

    const svLogicVecVal driven{0xa3u, 0x30u};
    cpptb_probe_set(&driven);
    svLogicVecVal observed{};
    cpptb_probe_get(&observed);
    const bool dpi_round_trip =
        observed.aval == driven.aval && observed.bval == driven.bval;

    std::printf(
        "CPPTB_FOUR_STATE_PROBE sv=%u net=%u dpi_from_sv=%u "
        "dpi_round_trip=%u\n",
        static_cast<unsigned>(sv_preserves),
        static_cast<unsigned>(net_resolves),
        static_cast<unsigned>(dpi_from_sv),
        static_cast<unsigned>(dpi_round_trip));
    std::fflush(stdout);
}
'''


@dataclass(frozen=True)
class VerilatorFourStateProbe:
    compiler_accepted: bool
    sv_values: bool
    net_resolution: bool
    dpi_from_sv: bool
    dpi_round_trip: bool
    output: str = ""

    @property
    def supported(self) -> bool:
        return (
            self.compiler_accepted
            and self.sv_values
            and self.net_resolution
            and self.dpi_from_sv
            and self.dpi_round_trip
        )

    def summary(self) -> str:
        def state(value: bool) -> str:
            return "available" if value else "unavailable"

        return (
            "Verilator --fourstate compilation: "
            f"{'accepted' if self.compiler_accepted else 'rejected'}; "
            f"SystemVerilog X/Z storage: {state(self.sv_values)}; "
            f"net X/Z resolution: {state(self.net_resolution)}; "
            f"DPI SV-to-C++ bval: {state(self.dpi_from_sv)}; "
            f"DPI round trip: {state(self.dpi_round_trip)}"
        )


def _probe_key(verilator: Sequence[str], verilator_version: str) -> str:
    digest = hashlib.sha256()
    digest.update(str(_PROBE_SCHEMA).encode())
    digest.update(json.dumps(list(verilator)).encode())
    digest.update(verilator_version.encode())
    digest.update(_SV_PROBE.encode())
    digest.update(_CPP_PROBE.encode())
    return digest.hexdigest()


def _load_cached(path: Path, key: str) -> VerilatorFourStateProbe | None:
    try:
        cached = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    if cached.get("schema_version") != _PROBE_SCHEMA or cached.get("key") != key:
        return None
    result = cached.get("result")
    if not isinstance(result, dict):
        return None
    try:
        return VerilatorFourStateProbe(**result)
    except TypeError:
        return None


def _write_cached(
    path: Path, key: str, result: VerilatorFourStateProbe
) -> None:
    path.write_text(
        json.dumps(
            {
                "schema_version": _PROBE_SCHEMA,
                "key": key,
                "result": asdict(result),
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )


def probe_verilator_four_state(
    verilator: Sequence[str],
    verilator_version: str,
    work_dir: Path,
    *,
    refresh: bool = False,
) -> VerilatorFourStateProbe:
    """Check behavior, not merely whether Verilator accepts --fourstate."""

    work_dir.mkdir(parents=True, exist_ok=True)
    key = _probe_key(verilator, verilator_version)
    cache_path = work_dir / "result.json"
    if not refresh:
        if cached := _load_cached(cache_path, key):
            return cached

    sv_source = work_dir / "probe.sv"
    cpp_source = work_dir / "probe.cpp"
    object_dir = work_dir / "obj"
    sv_source.write_text(_SV_PROBE, encoding="utf-8")
    cpp_source.write_text(_CPP_PROBE, encoding="utf-8")
    command = [
        *verilator,
        "--binary",
        "--timing",
        "--fourstate",
        "-Wno-FUTURE",
        "-Wno-MULTIDRIVEN",
        "--Mdir",
        os.fspath(object_dir),
        "--top-module",
        "cpptb_four_state_probe",
        os.fspath(sv_source),
        os.fspath(cpp_source),
    ]
    try:
        built = subprocess.run(
            command,
            cwd=work_dir,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
    except OSError as error:
        result = VerilatorFourStateProbe(
            False, False, False, False, False, str(error)
        )
        _write_cached(cache_path, key, result)
        return result

    output = built.stdout[-4000:]
    if built.returncode != 0:
        result = VerilatorFourStateProbe(
            False, False, False, False, False, output
        )
        _write_cached(cache_path, key, result)
        return result

    binary = object_dir / "Vcpptb_four_state_probe"
    try:
        ran = subprocess.run(
            [os.fspath(binary)],
            cwd=work_dir,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
    except OSError as error:
        result = VerilatorFourStateProbe(
            True, False, False, False, False, str(error)
        )
        _write_cached(cache_path, key, result)
        return result
    output = ran.stdout[-4000:]
    match = _RESULT_PATTERN.search(ran.stdout)
    if ran.returncode != 0 or match is None:
        result = VerilatorFourStateProbe(
            True, False, False, False, False, output
        )
    else:
        result = VerilatorFourStateProbe(
            True,
            match.group("sv") == "1",
            match.group("net") == "1",
            match.group("dpi_from_sv") == "1",
            match.group("dpi_round_trip") == "1",
            output,
        )
    _write_cached(cache_path, key, result)
    return result
