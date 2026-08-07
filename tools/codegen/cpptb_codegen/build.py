"""Build orchestration hidden behind the public cpptb command."""

from __future__ import annotations

import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

from cpptb_codegen.generate_dpi_bindings import generate_sources
from cpptb_codegen.project import ProjectSpec
from cpptb_codegen.verilator_capabilities import probe_verilator_four_state


class BuildError(RuntimeError):
    """A configured simulator build could not be completed."""


@dataclass(frozen=True)
class BuildResult:
    binary: Path
    rebuilt: bool


def find_framework_include(explicit_root: Path | None = None) -> Path:
    """Find the directory containing the installed cpptb/ headers."""

    candidates: list[Path] = []
    if explicit_root is not None:
        candidates.append(explicit_root.expanduser())
    configured = os.environ.get("CPPTB_ROOT")
    if configured:
        candidates.append(Path(configured).expanduser())
    module_path = Path(__file__).resolve()
    candidates.extend(module_path.parents)
    candidates.extend(
        [
            Path(sys.prefix) / "include",
            Path("/opt/homebrew/include"),
            Path("/usr/local/include"),
            Path("/usr/include"),
        ]
    )

    checked: list[Path] = []
    for candidate in candidates:
        for include in (candidate, candidate / "include"):
            resolved = include.resolve()
            if resolved in checked:
                continue
            checked.append(resolved)
            if (resolved / "cpptb" / "cpptb.hpp").is_file():
                return resolved
    raise BuildError(
        "cannot locate the cpptb C++ headers; install the CMake package, set "
        "CPPTB_ROOT, or pass --framework-root"
    )


def _tool_command(value: str, label: str) -> list[str]:
    command = shlex.split(value)
    if not command:
        raise BuildError(f"{label} command is empty")
    if shutil.which(command[0]) is None:
        raise BuildError(f"cannot find {label} executable: {command[0]}")
    return command


def _capture(command: Sequence[str], label: str) -> str:
    try:
        completed = subprocess.run(
            list(command),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
    except OSError as error:
        raise BuildError(f"cannot run {label}: {error}") from error
    if completed.returncode != 0:
        detail = completed.stdout.strip()
        raise BuildError(
            f"{label} exited with status {completed.returncode}"
            + (f":\n{detail}" if detail else "")
        )
    return completed.stdout.strip()


class _CommandLog:
    def __init__(self, path: Path, verbose: bool) -> None:
        self.path = path
        self.verbose = verbose
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("", encoding="utf-8")

    def run(self, command: Sequence[str], *, cwd: Path, label: str) -> None:
        rendered = shlex.join(os.fspath(item) for item in command)
        if self.verbose:
            print(f"$ {rendered}")
        try:
            completed = subprocess.run(
                [os.fspath(item) for item in command],
                cwd=cwd,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
        except OSError as error:
            raise BuildError(f"cannot run {label}: {error}") from error
        with self.path.open("a", encoding="utf-8") as output:
            output.write(f"$ {rendered}\n")
            output.write(completed.stdout)
            if completed.stdout and not completed.stdout.endswith("\n"):
                output.write("\n")
        if completed.returncode != 0:
            detail = completed.stdout.strip()
            raise BuildError(
                f"{label} failed with status {completed.returncode}; "
                f"see {self.path}"
                + (f"\n{detail}" if detail else "")
            )


def _hash_file(digest: "hashlib._Hash", path: Path) -> None:
    digest.update(os.fspath(path).encode())
    digest.update(b"\0")
    try:
        with path.open("rb") as source:
            while chunk := source.read(1024 * 1024):
                digest.update(chunk)
    except OSError as error:
        raise BuildError(f"cannot fingerprint {path}: {error}") from error


def _fingerprint(
    spec: ProjectSpec,
    framework_include: Path,
    verilator_version: str,
    cxx_version: str,
) -> str:
    digest = hashlib.sha256()
    settings = {
        "schema_version": 1,
        "root": str(spec.root),
        "top": spec.top,
        "target": spec.target,
        "build_name": spec.build_name,
        "simulator": spec.simulator,
        "include_dirs": [str(path) for path in spec.include_dirs],
        "defines": list(spec.defines),
        "parameters": list(spec.parameters),
        "testbench_include_dirs": [
            str(path) for path in spec.testbench_include_dirs
        ],
        "cxx_flags": list(spec.cxx_flags),
        # Part of the fingerprint so changing it in cpptb.toml rebuilds rather
        # than silently reusing objects compiled at the previous setting.
        "optimization": spec.optimization,
        "verilator_args": list(spec.verilator_args),
        "timing_backend": spec.timing_backend,
        "timeout_cycles": spec.timeout_cycles,
        "experimental_four_state": spec.experimental_four_state,
        "verilator_version": verilator_version,
        "cxx_version": cxx_version,
    }
    digest.update(json.dumps(settings, sort_keys=True).encode())
    files = [*spec.rtl_sources, *spec.testbench_sources]
    rtl_header_suffixes = {".svh", ".vh", ".sv", ".v"}
    cpp_header_suffixes = {".h", ".hh", ".hpp", ".hxx", ".inc"}
    for directory in spec.include_dirs:
        files.extend(
            path
            for path in directory.rglob("*")
            if path.is_file() and path.suffix.lower() in rtl_header_suffixes
        )
    for directory in {
        *(path.parent for path in spec.testbench_sources),
        *spec.testbench_include_dirs,
    }:
        files.extend(
            path
            for path in directory.rglob("*")
            if path.is_file() and path.suffix.lower() in cpp_header_suffixes
        )
    files.extend(
        sorted(
            path
            for path in (framework_include / "cpptb").rglob("*")
            if path.is_file()
            and path.suffix.lower() in {".hpp", ".sv", ".svh", ".cpp"}
        )
    )
    files.extend(sorted(Path(__file__).resolve().parent.rglob("*.py")))
    for path in sorted(dict.fromkeys(files)):
        _hash_file(digest, path)
    return digest.hexdigest()


def _state_matches(path: Path, fingerprint: str, binary: Path) -> bool:
    if not binary.is_file():
        return False
    try:
        state = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return False
    return state.get("schema_version") == 1 and state.get("fingerprint") == fingerprint


def _write_state(path: Path, spec: ProjectSpec, fingerprint: str) -> None:
    state = {
        "schema_version": 1,
        "fingerprint": fingerprint,
        "top": spec.top,
        "target": spec.target,
        "build_name": spec.build_name,
        "binary": str(spec.binary),
        "rtl_sources": [str(item) for item in spec.rtl_sources],
        "testbench_sources": [str(item) for item in spec.testbench_sources],
        "experimental_four_state": spec.experimental_four_state,
    }
    path.write_text(json.dumps(state, indent=2, sort_keys=True) + "\n", encoding="utf-8")


class VerilatorBackend:
    """The supported build backend; scheduling stays in generated DPI code."""

    def __init__(
        self,
        spec: ProjectSpec,
        *,
        framework_root: Path | None = None,
        verbose: bool = False,
    ) -> None:
        self.spec = spec
        self.framework_include = find_framework_include(framework_root)
        self.verbose = verbose
        self.verilator = _tool_command(
            os.environ.get("VERILATOR", "verilator"), "Verilator"
        )
        self.cxx = _tool_command(os.environ.get("CXX", "c++"), "C++ compiler")
        self.verilator_root = Path(
            _capture([*self.verilator, "--getenv", "VERILATOR_ROOT"], "Verilator")
        ).resolve()
        if not (self.verilator_root / "include" / "vltstd").is_dir():
            raise BuildError(
                f"Verilator reported an invalid VERILATOR_ROOT: {self.verilator_root}"
            )

    def build(
        self,
        *,
        rebuild: bool = False,
        compare_frontend: str | None = None,
    ) -> BuildResult:
        spec = self.spec
        verilator_version = _capture([*self.verilator, "--version"], "Verilator")
        if spec.experimental_four_state:
            probe = probe_verilator_four_state(
                self.verilator,
                verilator_version,
                spec.metadata_dir / "verilator-four-state-probe",
                refresh=rebuild,
            )
            if not probe.supported:
                raise BuildError(
                    "experimental four-state mode requested, but Verilator "
                    "does not preserve the required semantics\n"
                    f"  {probe.summary()}\n"
                    "Verilator's --fourstate flag is upstream-under-development. "
                    "Remove --experimental-four-state and use known 0/1 values, "
                    "or run four-state tests on a standards-compliant backend."
                )
            raise BuildError(
                "Verilator now passes the experimental four-state semantic "
                "probe, but CPPTB transport remains disabled until the full "
                "four-state conformance suite and performance peers pass. "
                "Update the capability regression and enablement review before "
                "using this mode."
            )
        cxx_version = _capture([*self.cxx, "--version"], "C++ compiler").splitlines()[0]
        fingerprint = _fingerprint(
            spec,
            self.framework_include,
            verilator_version,
            cxx_version,
        )
        state_path = spec.target_build_dir / "build-state.json"
        clock_config = spec.metadata_dir / "clocks.json"
        access_config = spec.metadata_dir / "access.json"
        sim_logging_dir = self.framework_include / "cpptb" / "sv"
        sim_log_package = sim_logging_dir / "cpptb_log_pkg.sv"
        sim_log_bridge = sim_logging_dir / "cpptb_sv_log_bridge.cpp"
        for asset in (sim_log_package, sim_log_bridge):
            if not asset.is_file():
                raise BuildError(f"cannot locate CPPTB simulation logging asset: {asset}")
        generation_options = {
            "top": spec.top,
            "output_dir": spec.generated_dir,
            "target": spec.target,
            "base_dir": spec.root,
            "include_dirs": list(spec.include_dirs),
            "defines": list(spec.defines),
            "parameters": spec.parameter_map,
            "timeout_cycles": spec.timeout_cycles,
        }
        if not rebuild and _state_matches(state_path, fingerprint, spec.binary):
            if compare_frontend is None:
                return BuildResult(spec.binary, False)
            if clock_config.is_file() and access_config.is_file():
                generate_sources(
                    list(spec.rtl_sources),
                    **generation_options,
                    clock_config=clock_config,
                    access_config=access_config,
                    compare_frontend=compare_frontend,
                )
                return BuildResult(spec.binary, False)

        spec.generated_dir.mkdir(parents=True, exist_ok=True)
        spec.metadata_dir.mkdir(parents=True, exist_ok=True)
        spec.object_dir.mkdir(parents=True, exist_ok=True)
        log = _CommandLog(spec.target_build_dir / "build.log", self.verbose)
        discovery = spec.target_build_dir / "discover_design"
        generate_sources(list(spec.rtl_sources), **generation_options)

        cpp_include_dirs = tuple(
            dict.fromkeys(
                [
                    self.framework_include,
                    spec.generated_dir,
                    spec.root,
                    *(path.parent for path in spec.testbench_sources),
                    *spec.testbench_include_dirs,
                    self.verilator_root / "include" / "vltstd",
                ]
            )
        )
        discovery_command = [
            *self.cxx,
            "-std=c++20",
            "-DCPPTB_HIERARCHY_DISCOVERY",
            *(f"-I{path}" for path in cpp_include_dirs),
            *spec.cxx_flags,
            str(spec.generated_dir / f"discover_{spec.target}_clocks.cpp"),
            *(str(path) for path in spec.testbench_sources),
            "-o",
            str(discovery),
        ]
        log.run(discovery_command, cwd=spec.root, label="testbench discovery compile")
        log.run(
            [str(discovery), str(clock_config), str(access_config)],
            cwd=spec.root,
            label="testbench discovery",
        )

        generate_sources(
            list(spec.rtl_sources),
            **generation_options,
            clock_config=clock_config,
            access_config=access_config,
            compare_frontend=compare_frontend,
        )

        # How the model is linked. Without a timing backend, Verilator's own
        # --binary main owns the loop and dispatches no phases. With one, the
        # framework host loop is linked instead, and one define picks whether
        # it drives Verilator's scheduler directly or rides VPI callbacks --
        # the same recipe the timing conformance suite builds and checks.
        timing_cflags: list[str] = []
        timing_sources: list[str] = []
        if spec.timing_backend:
            link_args = ["--cc", "--exe", "--build", "--vpi"]
            timing_main = (
                self.framework_include.parent / "src"
                / "verilator_timing_main.cpp"
            )
            if not timing_main.is_file():
                raise BuildError(
                    f"build.timing_backend needs the framework host loop at "
                    f"{timing_main}, which this cpptb installation does not "
                    f"ship; use a repository checkout"
                )
            timing_sources.append(str(timing_main))
            timing_cflags.append(f"-DCPPTB_VERILATED_TOP=V{spec.dpi_top}")
            if spec.timing_backend == "verilator-direct":
                timing_cflags.append("-DCPPTB_VERILATOR_DIRECT_TIMING")
        else:
            link_args = ["--binary"]

        cflags = shlex.join(
            [
                # Verilator adds neither a -std nor an optimization flag of its
                # own to testbench sources, so both would fall back to the
                # toolchain default: C++17 and no optimization. Placing them
                # first lets an explicit flag in cxx_flags still win.
                "-std=c++20",
                spec.optimization,
                *(f"-I{path}" for path in cpp_include_dirs if path != self.verilator_root / "include" / "vltstd"),
                *timing_cflags,
                *spec.cxx_flags,
            ]
        )
        verilator_command = [
            *self.verilator,
            *link_args,
            "--timing",
            "--no-sched-zero-delay",
            # OPT_FAST reaches only the model Verilator generates; the cflags
            # above cover the testbench. Both are needed for one setting to
            # actually govern the whole build.
            "-MAKEFLAGS",
            f"OPT_FAST={spec.optimization}",
            "-Wno-TIMESCALEMOD",
            "-Wno-WIDTH",
            "-Wno-UNUSEDSIGNAL",
            "-Wno-BLKANDNBLK",
            "-Wno-MULTIDRIVEN",
            *spec.verilator_args,
            f"-I{self.framework_include}",
            *(f"-I{path}" for path in spec.include_dirs),
            "-DCPPTB_ENABLE_SV_LOGGING",
            *(f"-D{value}" for value in spec.defines),
            # Parameters are not passed with -G. The generated wrapper is the
            # top module, and it already declares them as localparams and binds
            # them on the DUT instance, so -G would name parameters the top does
            # not have and Verilator rejects the build.
            "-CFLAGS",
            cflags,
            "--Mdir",
            str(spec.object_dir),
            "--top-module",
            spec.dpi_top,
            str(sim_log_package),
            *(str(path) for path in spec.rtl_sources),
            str(spec.generated_dir / f"dpi_{spec.target}.sv"),
            str(spec.generated_dir / f"dpi_{spec.target}.cpp"),
            str(sim_log_bridge),
            *(str(path) for path in spec.testbench_sources),
            *timing_sources,
        ]
        log.run(verilator_command, cwd=spec.root, label="Verilator build")
        if not spec.binary.is_file():
            raise BuildError(
                f"Verilator completed but did not produce the expected binary: {spec.binary}"
            )
        _write_state(state_path, spec, fingerprint)
        return BuildResult(spec.binary, True)


def build_project(
    spec: ProjectSpec,
    *,
    rebuild: bool = False,
    framework_root: Path | None = None,
    verbose: bool = False,
    compare_frontend: str | None = None,
) -> BuildResult:
    if spec.simulator != "verilator":
        raise BuildError(f"no build backend is available for {spec.simulator!r}")
    return VerilatorBackend(
        spec, framework_root=framework_root, verbose=verbose
    ).build(rebuild=rebuild, compare_frontend=compare_frontend)
