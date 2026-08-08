"""Project discovery and configuration for the public cpptb command."""

from __future__ import annotations

import re
import glob
import hashlib
import json
import os
import tomllib
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Sequence

from cpptb_codegen.design_ir import CodegenError
from cpptb_codegen.frontends.slang import infer_top_module


class ProjectError(RuntimeError):
    """A project cannot be resolved into an unambiguous build."""


@dataclass(frozen=True)
class ProjectSpec:
    root: Path
    rtl_sources: tuple[Path, ...]
    testbench_sources: tuple[Path, ...]
    top: str
    target: str
    build_name: str
    build_root: Path
    include_dirs: tuple[Path, ...] = ()
    defines: tuple[str, ...] = ()
    parameters: tuple[tuple[str, str | int], ...] = ()
    testbench_include_dirs: tuple[Path, ...] = ()
    cxx_flags: tuple[str, ...] = ()
    # Applies to both halves of the build: the C++ testbench and the model
    # Verilator generates. cpptb is a C++20 coroutine framework, and Verilator
    # optimizes neither by default, so an unset value would compile the
    # testbench unoptimized. Set "-O0" in cpptb.toml for a debug build.
    optimization: str = "-O2"
    verilator_args: tuple[str, ...] = ()
    # Simulation cycles after which the runtime declares a watchdog timeout.
    # Long software workloads need far more than the default: Ibex running
    # CoreMark takes about 4.1 million.
    timeout_cycles: int = 1_000_000
    experimental_four_state: bool = False
    simulator: str = "verilator"
    # Which mechanism resumes ReadWrite{}, ReadOnly{}, and NextTimeStep{}.
    # Empty means none: the default --binary build owns clocks and timers but
    # dispatches no phases, and a phase wait reports an actionable error at
    # run time. The two supported names both link the framework host loop and
    # hold the complete documented timing contract:
    #   "verilator-direct"  Verilator's scheduler driven directly; fastest.
    #   "vpi"               standard VPI callbacks; the portable route.
    timing_backend: str = ""
    # The cocotb write model: set() queues and flushes at the ReadWrite settle
    # point, set_now() stays immediate, and a get() in between reads the
    # simulator's value. Requires timing_backend, because the flush point is
    # the phase the backend dispatches.
    deferred_writes: bool = False
    # Waveform dumping: "" (off), "fst", or "vcd". A wave build instruments
    # the model (--trace-fst or --trace), which costs simulation speed even
    # when not dumping, so it is a build variant and never the default. The
    # host loop dumps when CPPTB_WAVE names an output file.
    wave: str = ""

    @property
    def target_build_dir(self) -> Path:
        return self.build_root / "cpptb" / self.build_name

    @property
    def generated_dir(self) -> Path:
        return self.target_build_dir / "generated"

    @property
    def metadata_dir(self) -> Path:
        return self.target_build_dir / "metadata"

    @property
    def object_dir(self) -> Path:
        return self.target_build_dir / "obj"

    @property
    def result_dir(self) -> Path:
        return self.target_build_dir / "results"

    @property
    def dpi_top(self) -> str:
        return f"dpi_{self.target}"

    @property
    def binary(self) -> Path:
        return self.object_dir / f"V{self.dpi_top}"

    @property
    def parameter_map(self) -> dict[str, str | int]:
        return dict(self.parameters)


def _project_root(path: Path | None) -> Path:
    root = (path or Path.cwd()).expanduser().resolve()
    if not root.is_dir():
        raise ProjectError(f"project directory does not exist: {root}")
    return root


def _load_config(root: Path) -> dict[str, Any]:
    path = root / "cpptb.toml"
    if not path.exists():
        return {}
    try:
        data = tomllib.loads(path.read_text(encoding="utf-8"))
    except (OSError, tomllib.TOMLDecodeError) as error:
        raise ProjectError(f"cannot read {path}: {error}") from error
    if not isinstance(data, dict):
        raise ProjectError(f"{path} must contain TOML tables")
    return data


def _table(config: dict[str, Any], name: str) -> dict[str, Any]:
    value = config.get(name, {})
    if not isinstance(value, dict):
        raise ProjectError(f"cpptb.toml [{name}] must be a table")
    return value


def _optimization(value: Any, label: str, default: str) -> str:
    """Validate build.optimization, which reaches both compilers as a flag."""
    if value is None:
        return default
    if not isinstance(value, str) or not re.fullmatch(r"-O[0-3sgz]", value):
        raise ProjectError(
            f"{label} must be a compiler optimization flag such as "
            '"-O2" or "-O0"'
        )
    return value


TIMING_BACKENDS = ("verilator-direct", "vpi")
WAVE_FORMATS = ("fst", "vcd")


def _wave(value: object) -> str:
    if value is None or value == "" or value is False:
        return ""
    if value is True:
        return "fst"
    if isinstance(value, str) and value in WAVE_FORMATS:
        return value
    names = ", ".join(f'"{name}"' for name in WAVE_FORMATS)
    raise ProjectError(
        f"build.wave must be true or one of {names}; got {value!r}"
    )

# The defines the timing machinery keys on. Setting any of them by hand can
# assemble a build that runs and answers wrongly -- measured: `--vpi` on the
# default main fails three of the five phase-contract checks with no
# diagnostic -- so they are owned by build.timing_backend and rejected
# everywhere else.
_TIMING_DEFINES = (
    "CPPTB_VERILATOR_DIRECT_TIMING",
    "CPPTB_VERILATED_TOP",
    "CPPTB_SV_DPI_TIMING",
    "CPPTB_SV_DPI_NBA_TIMING",
    "CPPTB_SV_DPI_CALENDAR_TIMING",
    "CPPTB_ALLOW_INVALID_TIMING",
)


def _timing_backend(value: Any) -> str:
    if value is None:
        return ""
    if not isinstance(value, str) or value not in TIMING_BACKENDS:
        names = ", ".join(f'"{name}"' for name in TIMING_BACKENDS)
        raise ProjectError(
            f"build.timing_backend must be one of {names}; got {value!r}. "
            f"Both link the framework host loop and hold the complete phase "
            f"contract; the sv-dpi experiments are not selectable here"
        )
    return value


def _reject_hand_rolled_timing(
    timing_backend: str,
    verilator_args: Sequence[str],
    defines: Sequence[str],
    cxx_flags: Sequence[str],
) -> None:
    """Phase dispatch is selected by name or not at all.

    A bare `--vpi` in verilator_args builds and runs, and fails three of the
    five phase-contract checks silently: `ReadOnly` does not observe a write
    settled in `ReadWrite`. The define combinations are guarded again at
    compile time in dpi_runtime.hpp, but the build tool says it first and
    names the key.
    """
    if timing_backend == "" and "--vpi" in verilator_args:
        raise ProjectError(
            "build.verilator_args must not pass --vpi: on the default main it "
            "builds a bridge that fails the phase timing contract silently. "
            'Set build.timing_backend = "vpi" (or "verilator-direct"), which '
            "emits the complete link"
        )
    if timing_backend and "--binary" in verilator_args:
        raise ProjectError(
            "build.verilator_args must not pass --binary when "
            "build.timing_backend is set; the backend owns the host loop and "
            "emits its own link"
        )
    for label, values in (("design.defines", defines),
                          ("build.cxx_flags", cxx_flags)):
        for value in values:
            if any(define in value for define in _TIMING_DEFINES):
                raise ProjectError(
                    f"{label} must not set {value!r}: the timing defines are "
                    f"owned by build.timing_backend so that an incomplete "
                    f"phase bridge cannot be assembled by hand"
                )


def _reject_optimization_in_verilator_args(values: Sequence[str]) -> None:
    """Keep one setting in charge of optimization.

    build.optimization already emits -MAKEFLAGS OPT_FAST, so a second one here
    would leave whichever Verilator happens to apply last in charge, and the
    two can disagree.
    """
    for value in values:
        if "OPT_FAST" in value:
            raise ProjectError(
                "build.verilator_args must not set OPT_FAST; use "
                "build.optimization, which applies to the testbench and the "
                "generated model together"
            )


def _timeout_cycles(value: Any, label: str, default: int) -> int:
    """Validate the watchdog cycle budget."""
    if value is None:
        return default
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        raise ProjectError(f"{label} must be a positive integer")
    return value


def _string_list(value: Any, label: str) -> list[str]:
    if value is None:
        return []
    if not isinstance(value, list) or not all(
        isinstance(item, str) and item for item in value
    ):
        raise ProjectError(f"{label} must be an array of non-empty strings")
    return list(value)


def _boolean(value: Any, label: str, default: bool = False) -> bool:
    if value is None:
        return default
    if not isinstance(value, bool):
        raise ProjectError(f"{label} must be a boolean")
    return value


def _resolve_patterns(
    root: Path,
    patterns: Sequence[str | Path],
    *,
    label: str,
    suffixes: frozenset[str],
) -> tuple[Path, ...]:
    matches: list[Path] = []
    for item in patterns:
        text = os.fspath(item)
        candidate = Path(text).expanduser()
        absolute_pattern = candidate if candidate.is_absolute() else root / candidate
        if glob.has_magic(os.fspath(absolute_pattern)):
            found = sorted(
                Path(path)
                for path in glob.glob(os.fspath(absolute_pattern), recursive=True)
            )
        elif absolute_pattern.is_dir():
            found = sorted(
                path
                for path in absolute_pattern.rglob("*")
                if path.is_file() and path.suffix.lower() in suffixes
            )
        else:
            found = [absolute_pattern]
        if not found:
            raise ProjectError(f"{label} pattern matched no files: {text}")
        for path in found:
            resolved = path.resolve()
            if not resolved.is_file():
                raise ProjectError(f"{label} file does not exist: {resolved}")
            if resolved.suffix.lower() not in suffixes:
                expected = ", ".join(sorted(suffixes))
                raise ProjectError(
                    f"{label} file has unsupported suffix: {resolved} "
                    f"(expected {expected})"
                )
            matches.append(resolved)
    return tuple(dict.fromkeys(matches))


def _discover_rtl(root: Path) -> tuple[Path, ...]:
    rtl_dir = root / "rtl"
    if rtl_dir.is_dir():
        sources = tuple(
            sorted(
                path.resolve()
                for path in rtl_dir.rglob("*")
                if path.is_file() and path.suffix.lower() in {".sv", ".v"}
            )
        )
        if sources:
            return sources
    return tuple(
        sorted(
            path.resolve()
            for path in root.iterdir()
            if path.is_file() and path.suffix.lower() in {".sv", ".v"}
        )
    )


def _discover_testbenches(root: Path) -> tuple[Path, ...]:
    tests_dir = root / "tests"
    if tests_dir.is_dir():
        sources = tuple(
            sorted(
                path.resolve()
                for path in tests_dir.rglob("*.cpp")
                if path.is_file()
            )
        )
        if sources:
            return sources
    flat = root / "testbench.cpp"
    return (flat.resolve(),) if flat.is_file() else ()


def _resolve_directories(
    root: Path, values: Iterable[str], label: str
) -> tuple[Path, ...]:
    directories: list[Path] = []
    for value in values:
        path = Path(value).expanduser()
        resolved = (path if path.is_absolute() else root / path).resolve()
        if not resolved.is_dir():
            raise ProjectError(f"{label} directory does not exist: {resolved}")
        directories.append(resolved)
    return tuple(dict.fromkeys(directories))


def _parameters(value: Any) -> tuple[tuple[str, str | int], ...]:
    if value is None:
        return ()
    if not isinstance(value, dict) or not all(
        isinstance(name, str)
        and name
        and isinstance(parameter, (str, int))
        and not isinstance(parameter, bool)
        for name, parameter in value.items()
    ):
        raise ProjectError(
            "cpptb.toml design.parameters must map names to strings or integers"
        )
    return tuple(sorted(value.items()))


def _build_root(root: Path, value: str | Path | None) -> Path:
    path = Path(value or "build").expanduser()
    return (path if path.is_absolute() else root / path).resolve()


def _top_cache_key(
    sources: Sequence[Path],
    include_dirs: Sequence[Path],
    defines: Sequence[str],
    parameters: Sequence[tuple[str, str | int]],
) -> str:
    digest = hashlib.sha256()
    digest.update(
        json.dumps(
            {
                "schema_version": 1,
                "defines": list(defines),
                "parameters": list(parameters),
            },
            sort_keys=True,
        ).encode()
    )
    files = list(sources)
    for directory in include_dirs:
        files.extend(
            path
            for path in directory.rglob("*")
            if path.is_file()
            and path.suffix.lower() in {".sv", ".svh", ".v", ".vh"}
        )
    files.extend(
        [
            Path(__file__).resolve(),
            (Path(__file__).resolve().parent / "frontends" / "slang.py"),
        ]
    )
    for path in sorted(dict.fromkeys(files)):
        digest.update(str(path).encode())
        digest.update(b"\0")
        digest.update(path.read_bytes())
    return digest.hexdigest()


def _infer_top_cached(
    *,
    root: Path,
    build_root: Path,
    sources: tuple[Path, ...],
    include_dirs: tuple[Path, ...],
    defines: tuple[str, ...],
    parameters: tuple[tuple[str, str | int], ...],
    refresh: bool,
) -> str:
    cache_path = build_root / "cpptb" / "project-cache.json"
    key = _top_cache_key(sources, include_dirs, defines, parameters)
    if not refresh:
        try:
            cache = json.loads(cache_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            cache = {}
        if cache.get("schema_version") == 1:
            cached = cache.get("tops", {}).get(key)
            if isinstance(cached, str) and cached:
                return cached

    inference_manifest = {
        "sources": [str(path) for path in sources],
        "include_dirs": [str(path) for path in include_dirs],
        "defines": list(defines),
        "parameters": dict(parameters),
        "frontend_options": {"slang": {"standard": "1800-2023"}},
    }
    try:
        selected_top = infer_top_module(inference_manifest, root)
    except CodegenError as error:
        raise ProjectError(str(error)) from error

    try:
        current = json.loads(cache_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        current = {}
    tops = current.get("tops", {}) if current.get("schema_version") == 1 else {}
    if not isinstance(tops, dict):
        tops = {}
    tops[key] = selected_top
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    cache_path.write_text(
        json.dumps({"schema_version": 1, "tops": tops}, indent=2, sort_keys=True)
        + "\n",
        encoding="utf-8",
    )
    return selected_top


def resolve_project(
    *,
    project: Path | None = None,
    sources: Sequence[Path] | None = None,
    testbenches: Sequence[Path] | None = None,
    top: str | None = None,
    target: str | None = None,
    build_name: str | None = None,
    build_dir: Path | None = None,
    simulator: str | None = None,
    experimental_four_state: bool | None = None,
    timing_backend: str | None = None,
    deferred_writes: bool | None = None,
    wave: str | None = None,
    refresh_top: bool = False,
) -> ProjectSpec:
    """Resolve CLI overrides, optional TOML, then filesystem conventions."""

    root = _project_root(project)
    config = _load_config(root)
    design = _table(config, "design")
    testbench = _table(config, "testbench")
    build = _table(config, "build")

    configured_sources = _string_list(design.get("sources"), "design.sources")
    if sources:
        rtl_sources = _resolve_patterns(
            root, sources, label="RTL source", suffixes=frozenset({".sv", ".v"})
        )
    elif configured_sources:
        rtl_sources = _resolve_patterns(
            root,
            configured_sources,
            label="RTL source",
            suffixes=frozenset({".sv", ".v"}),
        )
    else:
        rtl_sources = _discover_rtl(root)
    if not rtl_sources:
        raise ProjectError(
            f"no RTL sources found in {root}; add rtl/*.sv, a root-level .sv/.v "
            "file, design.sources in cpptb.toml, or --source"
        )

    configured_tests = _string_list(
        testbench.get("sources"), "testbench.sources"
    )
    if testbenches:
        testbench_sources = _resolve_patterns(
            root,
            testbenches,
            label="testbench source",
            suffixes=frozenset({".cpp", ".cc", ".cxx"}),
        )
    elif configured_tests:
        testbench_sources = _resolve_patterns(
            root,
            configured_tests,
            label="testbench source",
            suffixes=frozenset({".cpp", ".cc", ".cxx"}),
        )
    else:
        testbench_sources = _discover_testbenches(root)
    if not testbench_sources:
        raise ProjectError(
            f"no C++ testbench found in {root}; add tests/*.cpp or "
            "testbench.cpp, configure testbench.sources, or use --testbench"
        )

    include_dirs = _resolve_directories(
        root,
        _string_list(design.get("include_dirs"), "design.include_dirs"),
        "RTL include",
    )
    testbench_include_dirs = _resolve_directories(
        root,
        _string_list(testbench.get("include_dirs"), "testbench.include_dirs"),
        "testbench include",
    )
    defines = tuple(_string_list(design.get("defines"), "design.defines"))
    parameter_values = _parameters(design.get("parameters"))

    configured_build_dir = build.get("directory", "build")
    if not isinstance(configured_build_dir, str) or not configured_build_dir:
        raise ProjectError("cpptb.toml build.directory must be a non-empty string")
    resolved_build_root = _build_root(
        root, build_dir if build_dir is not None else configured_build_dir
    )

    selected_top = top or design.get("top")
    if selected_top is not None and not isinstance(selected_top, str):
        raise ProjectError("cpptb.toml design.top must be a string")
    if selected_top is None:
        selected_top = _infer_top_cached(
            root=root,
            build_root=resolved_build_root,
            sources=rtl_sources,
            include_dirs=include_dirs,
            defines=defines,
            parameters=parameter_values,
            refresh=refresh_top,
        )

    selected_target = target or build.get("target") or selected_top
    if not isinstance(selected_target, str) or not selected_target:
        raise ProjectError("cpptb.toml build.target must be a non-empty string")
    selected_build_name = build_name or build.get("name") or selected_target
    if not isinstance(selected_build_name, str) or not selected_build_name:
        raise ProjectError("cpptb.toml build.name must be a non-empty string")
    selected_simulator = simulator or build.get("simulator", "verilator")
    if selected_simulator != "verilator":
        raise ProjectError(
            f"unsupported simulator {selected_simulator!r}; the current build "
            "backend supports 'verilator'"
        )
    verilator_args = tuple(
        _string_list(build.get("verilator_args"), "build.verilator_args")
    )
    _reject_optimization_in_verilator_args(verilator_args)
    cxx_flags = tuple(_string_list(build.get("cxx_flags"), "build.cxx_flags"))
    # CLI overrides land before the cross-key validation, so a mode given on
    # the command line is checked against the merged configuration exactly as
    # if it had been written in the toml.
    selected_timing_backend = (
        _timing_backend(timing_backend)
        if timing_backend is not None
        else _timing_backend(build.get("timing_backend"))
    )
    _reject_hand_rolled_timing(selected_timing_backend, verilator_args,
                               defines, cxx_flags)
    selected_deferred = (
        deferred_writes
        if deferred_writes is not None
        else _boolean(
            build.get("deferred_writes"), "cpptb.toml build.deferred_writes"
        )
    )
    selected_wave = _wave(wave if wave is not None else build.get("wave"))
    if selected_wave and not selected_timing_backend:
        raise ProjectError(
            "build.wave needs build.timing_backend: the framework host loop "
            "owns the dump points, and only a timing backend links it. Set "
            'timing_backend = "verilator-direct" or "vpi"'
        )
    if selected_deferred and not selected_timing_backend:
        raise ProjectError(
            "build.deferred_writes needs build.timing_backend: the queued "
            "writes flush at the ReadWrite phase, which only a timing "
            'backend dispatches. Set timing_backend = "verilator-direct" '
            'or "vpi"'
        )
    if any(
        argument
        in {"--fourstate", "-fourstate", "--no-fourstate", "-no-fourstate"}
        for argument in verilator_args
    ):
        raise ProjectError(
            "build.verilator_args must not control '--fourstate'; use "
            "build.experimental_four_state so cpptb can run its semantic "
            "capability probe and reject silent X/Z coercion"
        )
    configured_four_state = _boolean(
        build.get("experimental_four_state"),
        "cpptb.toml build.experimental_four_state",
    )
    selected_four_state = (
        configured_four_state
        if experimental_four_state is None
        else experimental_four_state
    )

    return ProjectSpec(
        root=root,
        rtl_sources=rtl_sources,
        testbench_sources=testbench_sources,
        top=selected_top,
        target=selected_target,
        build_name=selected_build_name,
        build_root=resolved_build_root,
        include_dirs=include_dirs,
        defines=defines,
        parameters=parameter_values,
        testbench_include_dirs=testbench_include_dirs,
        cxx_flags=cxx_flags,
        optimization=_optimization(
            build.get("optimization"), "build.optimization",
            ProjectSpec.optimization,
        ),
        verilator_args=verilator_args,
        timeout_cycles=_timeout_cycles(
            _table(config, "run").get("timeout_cycles"),
            "run.timeout_cycles",
            ProjectSpec.timeout_cycles,
        ),
        experimental_four_state=selected_four_state,
        simulator=selected_simulator,
        timing_backend=selected_timing_backend,
        deferred_writes=selected_deferred,
        wave=selected_wave,
    )
