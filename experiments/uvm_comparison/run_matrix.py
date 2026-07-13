#!/usr/bin/env python3
"""Run the local UVM simulator comparison matrix.

The matrix intentionally keeps compile/build time separate from simulation
time. xezim is an interpreter, so every xezim run includes parse/elaborate and
simulation phases; Verilator has one build step followed by cheap binary runs.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BENCH = ROOT / "benchmarks" / "uvm-sim-compare"
DEPS = BENCH / "deps"
RESULTS = BENCH / "results"
GETTING = DEPS / "GettingVerilatorStartedWithUVM"
UVM_2017 = DEPS / "1800.2-2017-1.0" / "src"
UVM_MAIN = DEPS / "uvm-core" / "src"
XEZIM = DEPS / "xezim" / "target" / "release" / "xezim"

TESTS = ["data0_test", "data1_test", "random_test", "many_random_test"]
REPEATS = int(os.environ.get("BENCH_REPEATS", "3"))
TIMEOUT_S = int(os.environ.get("BENCH_TIMEOUT_S", "180"))


def run_command(name: str, cmd: list[str], cwd: Path, timeout_s: int = TIMEOUT_S) -> dict:
    RESULTS.mkdir(parents=True, exist_ok=True)
    log_path = RESULTS / f"{name}.log"
    started = datetime.now(timezone.utc).isoformat()
    t0 = time.perf_counter()
    try:
        completed = subprocess.run(
            cmd,
            cwd=cwd,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout_s,
        )
        output = completed.stdout
        rc = completed.returncode
        timed_out = False
    except subprocess.TimeoutExpired as exc:
        output = exc.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", "replace")
        output += f"\n[TIMEOUT after {timeout_s}s]\n"
        rc = 124
        timed_out = True
    wall = time.perf_counter() - t0
    log_path.write_text(output, encoding="utf-8")
    return {
        "name": name,
        "cmd": cmd,
        "cwd": str(cwd),
        "started": started,
        "returncode": rc,
        "timeout": timed_out,
        "wall_s": wall,
        "log": str(log_path.relative_to(ROOT)),
        "summary": parse_output(output),
    }


def parse_output(output: str) -> dict:
    summary: dict[str, object] = {}

    report_errors = re.findall(r"UVM_ERROR\s*:\s*(\d+)", output)
    report_fatals = re.findall(r"UVM_FATAL\s*:\s*(\d+)", output)
    if report_errors:
        summary["uvm_error_count"] = int(report_errors[-1])
    if report_fatals:
        summary["uvm_fatal_count"] = int(report_fatals[-1])

    summary["fatal_messages"] = re.findall(r"^UVM_FATAL\s+@[^\n]*", output, re.M)
    summary["error_messages"] = re.findall(r"^UVM_ERROR\s+@[^\n]*", output, re.M)
    summary["packet_counts"] = [int(x) for x in re.findall(r"COLLECTED PACKETS =\s*(\d+)", output)]

    x_phase = {}
    for phase, ms in re.findall(r"\[PHASE\]\s+([A-Za-z0-9_-]+):\s+([0-9.]+)ms", output):
        x_phase[phase] = float(ms) / 1000.0
    if x_phase:
        summary["xezim_phase_s"] = x_phase

    sim_times = re.findall(r"Simulation finished at time\s+(\d+)", output)
    if sim_times:
        summary["sim_finish_time"] = int(sim_times[-1])

    ver_build = re.search(r"Verilator: Walltime\s+([0-9.]+)\s+s.*allocated\s+([0-9.]+)\s+MB", output)
    if ver_build:
        summary["verilator_report_build_wall_s"] = float(ver_build.group(1))
        summary["verilator_report_alloc_mb"] = float(ver_build.group(2))

    ver_sim = re.search(
        r"Verilator: \$finish at\s+([^;]+);\s+walltime\s+([0-9.]+)\s+s;\s+speed\s+([0-9.]+)\s+([^ \n]+)",
        output,
    )
    if ver_sim:
        summary["verilator_finish_at"] = ver_sim.group(1).strip()
        summary["verilator_report_sim_wall_s"] = float(ver_sim.group(2))
        summary["verilator_report_speed"] = float(ver_sim.group(3))
        summary["verilator_report_speed_unit"] = ver_sim.group(4)

    coverage = re.findall(r"^\s*(line|toggle|branch|expr|fsm_state|fsm_arc)\s+:\s+([0-9.]+)%\s+\(\s*(\d+)/\s*(\d+)\)", output, re.M)
    if coverage:
        summary["coverage"] = {
            kind: {"pct": float(pct), "covered": int(covered), "total": int(total)}
            for kind, pct, covered, total in coverage
        }

    return summary


def xezim_cmd(test: str, uvm_src: Path) -> list[str]:
    return [
        str(XEZIM),
        "--simulate",
        "-s",
        "top",
        "-I",
        str(uvm_src),
        "-I",
        "rtl",
        "-I",
        "sv",
        "-I",
        "tb",
        "-D",
        "UVM_REPORT_DISABLE_FILE_LINE",
        "-D",
        "UVM_NO_DPI",
        "-D",
        "SVA_ON",
        str(uvm_src / "uvm_pkg.sv"),
        "sv/pipe_pkg.sv",
        "sv/pipe_if.sv",
        "rtl/pipe.v",
        "tb/top.sv",
        f"+UVM_TESTNAME={test}",
    ]


def verilator_binary() -> Path:
    return GETTING / "sim" / "obj_dir" / "Vuvm_pkg"


def git_rev(path: Path) -> str:
    out = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=path, text=True)
    return out.strip()


def mean(values: list[float]) -> float | None:
    return sum(values) / len(values) if values else None


def main() -> int:
    if len(sys.argv) > 1 and sys.argv[1] == "--summarize-existing":
        return summarize_existing()

    if not XEZIM.exists():
        raise SystemExit(f"missing xezim binary: {XEZIM}")
    if not UVM_2017.exists():
        raise SystemExit(f"missing UVM 2017 src dir: {UVM_2017}")

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    result: dict[str, object] = {
        "stamp": stamp,
        "repeats": REPEATS,
        "timeout_s": TIMEOUT_S,
        "tools": {
            "verilator": subprocess.check_output(["verilator", "--version"], text=True).strip(),
            "xezim": subprocess.check_output([str(XEZIM), "-V"], text=True).strip(),
        },
        "revisions": {
            "xezim": git_rev(DEPS / "xezim"),
            "xezim-core": git_rev(DEPS / "xezim-core"),
            "GettingVerilatorStartedWithUVM": git_rev(GETTING),
            "uvm-core": git_rev(DEPS / "uvm-core"),
        },
        "runs": [],
    }

    runs: list[dict] = result["runs"]  # type: ignore[assignment]

    # Compatibility probe against uvm-core main. It is intentionally not
    # repeated; the benchmark target uses Accellera 1800.2-2017-1.0.
    runs.append(
        run_command(
            "xezim_uvm-main_data0_probe",
            xezim_cmd("data0_test", UVM_MAIN),
            GETTING,
        )
    )

    for test in TESTS:
        for i in range(REPEATS):
            runs.append(
                run_command(
                    f"xezim_2017_{test}_r{i + 1}",
                    xezim_cmd(test, UVM_2017),
                    GETTING,
                )
            )

    sim_dir = GETTING / "sim"
    runs.append(
        run_command(
            "verilator_clean",
            ["make", f"UVM_HOME={UVM_2017}", "clean"],
            sim_dir,
            timeout_s=60,
        )
    )
    runs.append(
        run_command(
            "verilator_build",
            ["make", f"UVM_HOME={UVM_2017}", "obj_dir/Vuvm_pkg"],
            sim_dir,
            timeout_s=240,
        )
    )

    for test in TESTS:
        for i in range(REPEATS):
            cov_file = sim_dir / "logs" / "coverage" / f"{test}_r{i + 1}.cov"
            cov_file.parent.mkdir(parents=True, exist_ok=True)
            runs.append(
                run_command(
                    f"verilator_2017_{test}_r{i + 1}",
                    [
                        str(verilator_binary()),
                        f"+UVM_TESTNAME={test}",
                        f"+verilator+coverage+file+{cov_file}",
                    ],
                    sim_dir,
                    timeout_s=120,
                )
            )

    if shutil.which("verilator_coverage"):
        cov_files = sorted(str(p) for p in (sim_dir / "logs" / "coverage").glob("*.cov"))
        if cov_files:
            annotate = sim_dir / "logs" / "annotated_src"
            runs.append(
                run_command(
                    "verilator_coverage_annotate",
                    ["verilator_coverage", "--annotate", str(annotate), *cov_files],
                    sim_dir,
                    timeout_s=120,
                )
            )
            runs.append(
                run_command(
                    "verilator_coverage_rank",
                    ["verilator_coverage", "--rank", *cov_files],
                    sim_dir,
                    timeout_s=120,
                )
            )

    result["aggregates"] = aggregate(runs)

    json_path = RESULTS / "matrix_results.json"
    json_path.write_text(json.dumps(result, indent=2), encoding="utf-8")
    md_path = RESULTS / "matrix_summary.md"
    md_path.write_text(render_markdown(result), encoding="utf-8")
    print(json.dumps(result["aggregates"], indent=2))
    print(f"Wrote {json_path.relative_to(ROOT)}")
    print(f"Wrote {md_path.relative_to(ROOT)}")
    return 0


def summarize_existing() -> int:
    json_path = RESULTS / "matrix_results.json"
    if not json_path.exists():
        raise SystemExit(f"missing existing result file: {json_path}")
    result = json.loads(json_path.read_text(encoding="utf-8"))
    for run in result["runs"]:
        log_path = ROOT / run["log"]
        output = log_path.read_text(encoding="utf-8") if log_path.exists() else ""
        run["summary"] = parse_output(output)
    result["aggregates"] = aggregate(result["runs"])
    json_path.write_text(json.dumps(result, indent=2), encoding="utf-8")
    md_path = RESULTS / "matrix_summary.md"
    md_path.write_text(render_markdown(result), encoding="utf-8")
    print(json.dumps(result["aggregates"], indent=2))
    print(f"Updated {json_path.relative_to(ROOT)}")
    print(f"Updated {md_path.relative_to(ROOT)}")
    return 0


def is_pass(run: dict) -> bool:
    if run["returncode"] != 0 or run["timeout"]:
        return False
    summary = run.get("summary", {})
    if summary.get("uvm_error_count", 0) != 0:
        return False
    if summary.get("uvm_fatal_count", 0) != 0:
        return False
    if summary.get("fatal_messages"):
        return False
    return True


def aggregate(runs: list[dict]) -> dict:
    out: dict[str, object] = {}
    for sim in ("xezim_2017", "verilator_2017"):
        table = {}
        for test in TESTS:
            selected = [r for r in runs if r["name"].startswith(f"{sim}_{test}_")]
            table[test] = {
                "runs": len(selected),
                "passes": sum(1 for r in selected if is_pass(r)),
                "mean_wall_s": mean([r["wall_s"] for r in selected]),
                "wall_s": [r["wall_s"] for r in selected],
                "packet_counts": [r["summary"].get("packet_counts", []) for r in selected],
                "sim_finish_times": [r["summary"].get("sim_finish_time") for r in selected if "sim_finish_time" in r["summary"]],
            }
            if sim == "xezim_2017":
                table[test]["mean_xezim_total_phase_s"] = mean(
                    [
                        r["summary"].get("xezim_phase_s", {}).get("total")
                        for r in selected
                        if r["summary"].get("xezim_phase_s", {}).get("total") is not None
                    ]
                )
                table[test]["mean_xezim_compile_phase_s"] = mean(
                    [
                        r["summary"].get("xezim_phase_s", {}).get("compilation")
                        for r in selected
                        if r["summary"].get("xezim_phase_s", {}).get("compilation") is not None
                    ]
                )
                table[test]["mean_xezim_sim_phase_s"] = mean(
                    [
                        r["summary"].get("xezim_phase_s", {}).get("simulation")
                        for r in selected
                        if r["summary"].get("xezim_phase_s", {}).get("simulation") is not None
                    ]
                )
            if sim == "verilator_2017":
                table[test]["mean_verilator_report_sim_wall_s"] = mean(
                    [
                        r["summary"].get("verilator_report_sim_wall_s")
                        for r in selected
                        if r["summary"].get("verilator_report_sim_wall_s") is not None
                    ]
                )
        out[sim] = table

    build = next((r for r in runs if r["name"] == "verilator_build"), None)
    if build:
        out["verilator_build"] = {
            "pass": is_pass(build),
            "wall_s": build["wall_s"],
            "reported_wall_s": build["summary"].get("verilator_report_build_wall_s"),
            "reported_alloc_mb": build["summary"].get("verilator_report_alloc_mb"),
            "log": build["log"],
        }
    uvm_main = next((r for r in runs if r["name"] == "xezim_uvm-main_data0_probe"), None)
    if uvm_main:
        out["xezim_uvm_main_probe"] = {
            "pass": is_pass(uvm_main),
            "wall_s": uvm_main["wall_s"],
            "fatal_messages": uvm_main["summary"].get("fatal_messages", []),
            "log": uvm_main["log"],
        }
    coverage = next((r for r in runs if r["name"] == "verilator_coverage_annotate"), None)
    if coverage:
        out["verilator_coverage"] = coverage["summary"].get("coverage", {})
    return out


def render_markdown(result: dict) -> str:
    lines = [
        "# UVM Simulator Matrix Results",
        "",
        f"- Timestamp: `{result['stamp']}`",
        f"- Repeats: `{result['repeats']}`",
        f"- Verilator: `{result['tools']['verilator']}`",
        f"- xezim: `{result['tools']['xezim']}`",
        "",
        "## Revisions",
        "",
    ]
    for name, rev in result["revisions"].items():
        lines.append(f"- {name}: `{rev}`")
    lines += ["", "## Aggregate Results", ""]
    lines += render_separated_comparison(result["aggregates"])
    lines.append("")
    lines.append("### xezim, Accellera 1800.2-2017-1.0")
    lines += render_sim_table(result["aggregates"]["xezim_2017"], xezim=True)
    lines += ["", "### Verilator, Accellera 1800.2-2017-1.0"]
    lines += render_sim_table(result["aggregates"]["verilator_2017"], xezim=False)
    lines += ["", "### Verilator Build"]
    lines.append(f"```json\n{json.dumps(result['aggregates'].get('verilator_build', {}), indent=2)}\n```")
    lines += ["", "### Verilator Code Coverage"]
    lines.append(f"```json\n{json.dumps(result['aggregates'].get('verilator_coverage', {}), indent=2)}\n```")
    lines += ["", "### xezim Probe Against uvm-core main"]
    lines.append(f"```json\n{json.dumps(result['aggregates'].get('xezim_uvm_main_probe', {}), indent=2)}\n```")
    lines += ["", "## Raw Runs", ""]
    for run in result["runs"]:
        lines.append(
            f"- `{run['name']}` rc={run['returncode']} wall={run['wall_s']:.3f}s "
            f"timeout={run['timeout']} log=`{run['log']}`"
        )
    lines.append("")
    return "\n".join(lines)


def render_separated_comparison(aggregates: dict) -> list[str]:
    lines = ["### Build-Only Comparison", ""]
    xezim_compile = [
        row["mean_xezim_compile_phase_s"]
        for row in aggregates["xezim_2017"].values()
        if row.get("mean_xezim_compile_phase_s") is not None
    ]
    x_compile_mean = mean(xezim_compile)
    v_build = aggregates.get("verilator_build", {})
    v_build_wall = v_build.get("wall_s")
    build_ratio = (v_build_wall / x_compile_mean) if v_build_wall is not None and x_compile_mean else None
    ratio_text = f"{build_ratio:.1f}x" if build_ratio else "n/a"
    lines += [
        "| Simulator | What This Measures | Mean/Wall s | Notes |",
        "|---|---|---:|---|",
        f"| xezim | Per-run parse/elaboration phase | {x_compile_mean:.3f} | No generated design binary; paid each run. |",
        f"| Verilator | One-time generated C++ build | {v_build_wall:.3f} | Reused for all subsequent test runs. |",
        "",
        f"Build ratio, Verilator build / xezim parse-elab: `{ratio_text}`. This is an iteration-cost comparison, not an artifact-equivalence claim.",
        "",
        "### Runtime-Only Comparison",
        "",
        "| Test | Functional Status | xezim sim phase s | Verilator sim report s | Runtime Ratio |",
        "|---|---|---:|---:|---:|",
    ]
    for test, x_row in aggregates["xezim_2017"].items():
        v_row = aggregates["verilator_2017"][test]
        x_sim = x_row.get("mean_xezim_sim_phase_s")
        v_sim = v_row.get("mean_verilator_report_sim_wall_s")
        ratio = (x_sim / v_sim) if x_sim is not None and v_sim else None
        ratio_text = f"{ratio:.1f}x" if ratio else "n/a"
        status = f"xezim {x_row['passes']}/{x_row['runs']}; Verilator {v_row['passes']}/{v_row['runs']}"
        lines.append(f"| {test} | {status} | {x_sim:.3f} | {v_sim:.3f} | {ratio_text} |")
    lines += [
        "",
        "The cleanest apples-to-apples runtime rows are `data0_test` after rerun and `data1_test`; the random tests execute under Verilator but report scoreboard UVM errors.",
    ]
    return lines


def render_sim_table(table: dict, xezim: bool) -> list[str]:
    if xezim:
        header = "| Test | Passes | Mean wall s | xezim compile s | xezim sim s | xezim total s | Packets | Finish times |"
        sep = "|---|---:|---:|---:|---:|---:|---|---|"
    else:
        header = "| Test | Passes | Mean wall s | Verilator report sim wall s | Packets | Finish times |"
        sep = "|---|---:|---:|---:|---|---|"
    lines = [header, sep]
    for test, row in table.items():
        packets = json.dumps(row.get("packet_counts", []))
        finishes = json.dumps(row.get("sim_finish_times", []))
        passes = f"{row['passes']}/{row['runs']}"
        mean_wall = row["mean_wall_s"]
        if xezim:
            lines.append(
                f"| {test} | {passes} | {mean_wall:.3f} | "
                f"{row['mean_xezim_compile_phase_s']:.3f} | {row['mean_xezim_sim_phase_s']:.3f} | "
                f"{row['mean_xezim_total_phase_s']:.3f} | `{packets}` | `{finishes}` |"
            )
        else:
            rep = row.get("mean_verilator_report_sim_wall_s")
            rep_text = f"{rep:.3f}" if rep is not None else "n/a"
            lines.append(
                f"| {test} | {passes} | {mean_wall:.3f} | "
                f"{rep_text} | `{packets}` | `{finishes}` |"
            )
    return lines


if __name__ == "__main__":
    raise SystemExit(main())
