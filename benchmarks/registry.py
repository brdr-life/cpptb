"""Central, side-effect-free metadata for benchmark discovery and execution."""

from __future__ import annotations

import re
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import Iterable


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MAKEFILE = REPO_ROOT / "Makefile"


class Category(str, Enum):
    AUTHORING_FEATURE = "authoring_feature"
    INTEGRATION = "integration"


class AdapterKind(str, Enum):
    AUTHORING_CORE = "authoring_core"
    DPI_APB_REGFILE = "dpi_apb_regfile"
    DPI_APB_TRACE = "dpi_apb_trace"
    DPI_COMPONENT_FIFO = "dpi_component_fifo"
    DPI_COUNTER = "dpi_counter"
    DPI_FAULT_INJECTION = "dpi_fault_injection"
    DPI_FIFO_SCOREBOARD = "dpi_fifo_scoreboard"
    DPI_INTERFACES = "dpi_interfaces"
    DPI_MULTICLOCK = "dpi_multiclock"
    DPI_RICH_DATA = "dpi_rich_data"
    DPI_TIMER_ONLY = "dpi_timer_only"
    DPI_WATCHDOG_TIMEOUT = "dpi_watchdog_timeout"
    PERIPHERAL_SUITE = "peripheral_suite"


class GatePolicy(str, Enum):
    HARD_1_10 = "hard_1_10"
    WAIVED_HARD_1_10 = "waived_hard_1_10"
    EQUIVALENCE_ONLY = "equivalence_only"
    DIAGNOSTIC = "diagnostic"


@dataclass(frozen=True)
class Binary:
    adapter: str
    path: str


@dataclass(frozen=True)
class Runner:
    """Commands and argument conventions understood by an orchestration layer."""

    commands: tuple[tuple[str, ...], ...]
    iterations_argument: str | None = None
    iterations_environment: str | None = None
    kernel_argument: str | None = None
    semantic_build_targets: tuple[str, ...] | None = None


@dataclass(frozen=True)
class PerformanceWaiver:
    approved_on: str
    max_ratio: float
    rationale: str


@dataclass(frozen=True)
class Benchmark:
    name: str
    label: str
    category: Category
    adapter_kind: AdapterKind
    build_targets: tuple[str, ...]
    binaries: tuple[Binary, ...]
    runner: Runner
    default_iterations: int
    gate_policy: GatePolicy
    template_id: int | None = None
    waiver: PerformanceWaiver | None = None

    @property
    def binary_paths(self) -> tuple[str, ...]:
        return tuple(binary.path for binary in self.binaries)


_AUTHORING_TEMPLATE_IDS = {
    "control": 0,
    "task_value": 1,
    "clock_cycles": 2,
    "timeout": 3,
    "task_timeout": 8,
    "wait_until": 4,
    "event": 5,
    "queue": 6,
    "all": 7,
    "wide64": 9,
    "wide_echo_137": 10,
    "wide_slice": 11,
    "fixed_mac": 12,
    "array_index": 13,
    "array_wide": 14,
    "mem_rw": 15,
    "hier_probe": 16,
    "mem_backdoor": 17,
    "mem_probe_read": 18,
    "mem_probe_deposit": 19,
    "mem_probe_read_deposit": 20,
    "signal_edge": 21,
    "array_multidim": 22,
    "force_release": 23,
    "packed_view": 24,
    "force_direct": 25,
    "hier_data": 26,
    "timing_phases": 27,
    "timing_phases_deferred": 27,
    "queue_sync": 28,
    "test_lifecycle": 29,
    "dynamic_spawn": 30,
    "dynamic_task": 31,
    "dynamic_spawn_scheduler": 32,
    "dynamic_spawn_suspending": 33,
    "dynamic_monitor": 34,
    "analysis_fanout": 35,
    "random_stimulus": 36,
    "constrained_packet": 37,
    "constraint_extensions": 38,
    "coverage_sampling": 39,
    "apb_component": 40,
    "transaction_recording": 58,
    "process_pipeline": 41,
    "memory_model": 42,
    "memory_model_direct": 43,
    "register_prediction_validity": 44,
    "register_backdoor": 45,
    "register_hierarchy": 46,
    "register_split": 47,
    "register_wide": 48,
    "register_enum": 49,
    "register_memory": 50,
    "register_sequences": 51,
    "register_coverage": 52,
    "register_maps": 53,
    "register_user_effects": 54,
    "structured_logging": 55,
    "structured_log_history": 56,
    "mixed_logging": 57,
}


def _authoring(
    name: str,
    label: str,
    *,
    default_iterations: int = 100_000,
    gate_policy: GatePolicy = GatePolicy.HARD_1_10,
    waiver: PerformanceWaiver | None = None,
) -> Benchmark:
    dpi_binary = f"build/benchmarks/authoring_core/cpp_dpi_{name}/Vdpi_authoring_core"
    if name == "force_direct":
        sv_target = "authoring-core-force-direct-sv-build"
        sv_binary = (
            "build/benchmarks/authoring_core/force_direct_sv_obj/"
            "Vforce_direct_sv_tb"
        )
    else:
        sv_target = "authoring-core-sv-build"
        sv_binary = (
            "build/benchmarks/authoring_core/pure_sv_obj/"
            "Vauthoring_core_sv_tb"
        )
    return Benchmark(
        name=name,
        label=label,
        category=Category.AUTHORING_FEATURE,
        adapter_kind=AdapterKind.AUTHORING_CORE,
        build_targets=(dpi_binary, sv_target),
        binaries=(
            Binary(
                "cpp_dpi",
                dpi_binary,
            ),
            Binary(
                "pure_sv",
                sv_binary,
            ),
        ),
        runner=Runner(
            commands=(("python3", "benchmarks/authoring_core/run_benchmark.py"),),
            iterations_argument="--iters",
            iterations_environment="AUTHORING_CORE_ITERS",
            kernel_argument="--example",
        ),
        default_iterations=default_iterations,
        gate_policy=gate_policy,
        template_id=_AUTHORING_TEMPLATE_IDS[name],
        waiver=waiver,
    )


BENCHMARKS: tuple[Benchmark, ...] = (
    _authoring("control", "Control"),
    _authoring("task_value", "Task value"),
    _authoring("clock_cycles", "Clock cycles"),
    _authoring("timeout", "Timeout"),
    _authoring("task_timeout", "Task timeout"),
    _authoring("wait_until", "Wait until"),
    _authoring("event", "Event"),
    _authoring("queue", "Queue"),
    _authoring("queue_sync", "Bounded queue and synchronization"),
    _authoring("all", "All authoring features"),
    _authoring("wide64", "64-bit packed signal"),
    _authoring("wide_echo_137", "137-bit packed signal"),
    _authoring("wide_slice", "Wide packed slices"),
    _authoring("fixed_mac", "Fixed-point multiply"),
    _authoring("array_index", "Unpacked array indexing"),
    _authoring("array_wide", "Wide unpacked array"),
    _authoring("mem_rw", "Synchronous memory front door"),
    _authoring("hier_probe", "Hierarchical internal probe"),
    _authoring("mem_backdoor", "Internal memory backdoor"),
    _authoring("mem_probe_read", "Internal memory read attribution"),
    _authoring("mem_probe_deposit", "Internal memory deposit attribution"),
    _authoring(
        "mem_probe_read_deposit",
        "Internal memory read/deposit attribution",
    ),
    _authoring("signal_edge", "DUT output edge observer"),
    _authoring("array_multidim", "Multidimensional unpacked array"),
    _authoring("force_release", "Internal net force/release"),
    _authoring("packed_view", "Packed enum/struct views"),
    _authoring(
        "force_direct",
        "Zero-time force/get/release",
        gate_policy=GatePolicy.WAIVED_HARD_1_10,
        waiver=PerformanceWaiver(
            approved_on="2026-07-14",
            max_ratio=1.20,
            rationale=(
                "The isolated zero-time force/read/release microbenchmark remains "
                "transport-bound at 1.135x after eliminating the redundant read DPI "
                "call. It has no scheduler resume or time advance, and the waiver "
                "applies only to this direct exported-DPI force path."
            ),
        ),
    ),
    _authoring("hier_data", "Wide and four-state hierarchy data"),
    _authoring("timing_phases", "Simulator timing phases"),
    _authoring(
        "timing_phases_deferred",
        "Simulator timing phases, deferred writes",
    ),
    _authoring(
        "test_lifecycle", "Test lifecycle checks", default_iterations=5_000_000
    ),
    _authoring(
        "dynamic_spawn",
        "Immediate dynamic process creation (diagnostic control)",
        default_iterations=5_000_000,
        gate_policy=GatePolicy.DIAGNOSTIC,
    ),
    _authoring(
        "dynamic_task",
        "Repeated direct task composition (diagnostic control)",
        default_iterations=5_000_000,
        gate_policy=GatePolicy.DIAGNOSTIC,
    ),
    _authoring(
        "dynamic_spawn_scheduler",
        "Repeated low-level scheduler process creation",
        default_iterations=5_000_000,
    ),
    _authoring(
        "dynamic_spawn_suspending",
        "Repeated suspending process creation",
        default_iterations=5_000_000,
    ),
    _authoring(
        "dynamic_monitor",
        "Long-lived monitor processes",
    ),
    _authoring(
        "process_pipeline",
        "Finite driver, worker, and scoreboard processes",
    ),
    _authoring(
        "analysis_fanout",
        "Transaction analysis fan-out",
    ),
    _authoring(
        "random_stimulus",
        "Deterministic random stimulus",
    ),
    _authoring(
        "constrained_packet",
        "Constrained-random packet stimulus",
    ),
    _authoring(
        "constraint_extensions",
        "Extended constrained-random fields and policies",
    ),
    _authoring(
        "coverage_sampling",
        "Functional coverage sampling",
    ),
    _authoring(
        "apb_component",
        "APB verification components",
    ),
    _authoring(
        "transaction_recording",
        "Typed transaction recording",
    ),
    _authoring(
        "memory_model",
        "Sparse memory prediction",
    ),
    _authoring(
        "memory_model_direct",
        "Direct sparse memory model",
    ),
    _authoring(
        "register_prediction_validity",
        "Register prediction validity",
    ),
    _authoring(
        "register_backdoor",
        "Generated register backdoor",
    ),
    _authoring(
        "register_hierarchy",
        "Generated register hierarchy",
    ),
    _authoring(
        "register_split",
        "Split-width register frontdoor",
    ),
    _authoring(
        "register_wide",
        "Arbitrary-width register frontdoor",
    ),
    _authoring(
        "register_enum",
        "Typed register field enumeration",
    ),
    _authoring(
        "register_memory",
        "Generated register-backed memory access",
        default_iterations=10_000_000,
    ),
    _authoring(
        "register_sequences",
        "Standard register sequences",
    ),
    _authoring(
        "register_coverage",
        "Passive register access coverage",
        # Both sides run a descriptor-driven collector that derives its
        # tallies from the observed transactions through a per-address
        # action table precomputed at construction. The pure-SV peer was
        # rewritten from a hand-solved answer key to that engine shape in
        # 2026-08 (the C++ side gained the table in the same pass), which
        # is what returned this entry to the ordinary hard gate. Ten
        # million iterations keep both sides' samples comfortably above
        # the CPU-corroboration noise floor.
        default_iterations=10_000_000,
    ),
    _authoring(
        "register_maps",
        "Register address maps and custom frontdoors",
    ),
    _authoring(
        "register_user_effects",
        "Custom register read and write effects",
        default_iterations=10_000_000,
        gate_policy=GatePolicy.DIAGNOSTIC,
    ),
    _authoring(
        "structured_logging",
        "Structured process-aware logging",
        default_iterations=5_000_000,
    ),
    _authoring(
        "structured_log_history",
        "Ordered structured log history",
        default_iterations=5_000_000,
    ),
    _authoring(
        "mixed_logging",
        "Mixed C++ and SystemVerilog structured logging",
        default_iterations=5_000_000,
    ),
    Benchmark(
        name="dpi_counter",
        label="DPI counter",
        category=Category.INTEGRATION,
        adapter_kind=AdapterKind.DPI_COUNTER,
        build_targets=("cpp-dpi-counter-build", "cpp-dpi-counter-sv-build"),
        binaries=(
            Binary("cpp_dpi", "build/cpptb/counter/obj/Vdpi_counter"),
            Binary("pure_sv", "build/cpptb/counter/systemverilog_obj/Vcounter_sv_tb"),
        ),
        runner=Runner(
            commands=(
                ("make", "cpp-dpi-counter-run"),
                ("make", "cpp-dpi-counter-sv-run"),
            ),
        ),
        default_iterations=1,
        gate_policy=GatePolicy.EQUIVALENCE_ONLY,
    ),
    Benchmark(
        name="dpi_multiclock",
        label="DPI multiclock",
        category=Category.INTEGRATION,
        adapter_kind=AdapterKind.DPI_MULTICLOCK,
        build_targets=("cpp-dpi-multiclock-build", "cpp-dpi-multiclock-sv-build"),
        binaries=(
            Binary("cpp_dpi", "build/cpptb/multiclock/obj/Vdpi_dual_clock_mailbox"),
            Binary(
                "pure_sv",
                "build/cpptb/multiclock/systemverilog_obj/Vdual_clock_mailbox_sv_tb",
            ),
        ),
        runner=Runner(
            commands=(
                ("make", "cpp-dpi-multiclock-run"),
                ("make", "cpp-dpi-multiclock-sv-run"),
            ),
        ),
        default_iterations=1,
        gate_policy=GatePolicy.EQUIVALENCE_ONLY,
    ),
    Benchmark(
        name="dpi_timer_only",
        label="DPI timer only",
        category=Category.INTEGRATION,
        adapter_kind=AdapterKind.DPI_TIMER_ONLY,
        build_targets=(
            "cpp-dpi-timer-only-build",
            "cpp-dpi-timer-only-sv-build",
        ),
        binaries=(
            Binary(
                "cpp_dpi",
                "build/cpptb/timer_only/obj/Vdpi_timer_only_probe",
            ),
            Binary(
                "pure_sv",
                "build/cpptb/timer_only/systemverilog_obj/Vtimer_only_probe_sv_tb",
            ),
        ),
        runner=Runner(
            commands=(
                ("make", "cpp-dpi-timer-only-run"),
                ("make", "cpp-dpi-timer-only-sv-run"),
            ),
        ),
        default_iterations=1,
        gate_policy=GatePolicy.EQUIVALENCE_ONLY,
    ),
    Benchmark(
        name="dpi_fifo_scoreboard",
        label="DPI FIFO scoreboard",
        category=Category.INTEGRATION,
        adapter_kind=AdapterKind.DPI_FIFO_SCOREBOARD,
        build_targets=(
            "cpp-dpi-fifo-scoreboard-build",
            "cpp-dpi-fifo-scoreboard-sv-build",
        ),
        binaries=(
            Binary(
                "cpp_dpi",
                "build/cpptb/fifo_scoreboard/obj/Vdpi_stream_fifo",
            ),
            Binary(
                "pure_sv",
                "build/cpptb/fifo_scoreboard/systemverilog_obj/Vstream_fifo_sv_tb",
            ),
        ),
        runner=Runner(
            commands=(
                ("make", "cpp-dpi-fifo-scoreboard-run"),
                ("make", "cpp-dpi-fifo-scoreboard-sv-run"),
            ),
        ),
        default_iterations=1,
        gate_policy=GatePolicy.EQUIVALENCE_ONLY,
    ),
    Benchmark(
        name="dpi_component_fifo",
        label="DPI component FIFO",
        category=Category.INTEGRATION,
        adapter_kind=AdapterKind.DPI_COMPONENT_FIFO,
        build_targets=(
            "cpp-dpi-component-fifo-build",
            "cpp-dpi-component-fifo-sv-build",
        ),
        binaries=(
            Binary(
                "cpp_dpi",
                "build/cpptb/component_fifo/obj/Vdpi_component_fifo",
            ),
            Binary(
                "pure_sv",
                "build/cpptb/component_fifo/systemverilog_obj/"
                "Vcomponent_fifo_sv_tb",
            ),
        ),
        runner=Runner(
            commands=(
                ("make", "cpp-dpi-component-fifo-run"),
                ("make", "cpp-dpi-component-fifo-sv-run"),
            ),
        ),
        default_iterations=1,
        gate_policy=GatePolicy.EQUIVALENCE_ONLY,
    ),
    Benchmark(
        name="dpi_apb_regfile",
        label="DPI APB register file",
        category=Category.INTEGRATION,
        adapter_kind=AdapterKind.DPI_APB_REGFILE,
        build_targets=(
            "cpp-dpi-apb-regfile-build",
            "cpp-dpi-apb-regfile-sv-build",
        ),
        binaries=(
            Binary(
                "cpp_dpi",
                "build/cpptb/apb_regfile/obj/Vdpi_apb_regfile",
            ),
            Binary(
                "pure_sv",
                "build/cpptb/apb_regfile/systemverilog_obj/Vapb_regfile_sv_tb",
            ),
        ),
        runner=Runner(
            commands=(
                ("make", "cpp-dpi-apb-regfile-run"),
                ("make", "cpp-dpi-apb-regfile-sv-run"),
            ),
        ),
        default_iterations=1,
        gate_policy=GatePolicy.EQUIVALENCE_ONLY,
    ),
    Benchmark(
        name="dpi_apb_trace",
        label="DPI APB transaction trace",
        category=Category.INTEGRATION,
        adapter_kind=AdapterKind.DPI_APB_TRACE,
        build_targets=(
            "cpp-dpi-apb-trace-build",
            "cpp-dpi-apb-trace-sv-build",
        ),
        binaries=(
            Binary(
                "cpp_dpi",
                "build/cpptb/apb_trace/obj/Vdpi_apb_trace",
            ),
            Binary(
                "pure_sv",
                "build/cpptb/apb_trace/systemverilog_obj/Vapb_trace_sv_tb",
            ),
        ),
        runner=Runner(
            commands=(
                ("make", "cpp-dpi-apb-trace-run"),
                ("make", "cpp-dpi-apb-trace-sv-run"),
            ),
        ),
        default_iterations=1,
        gate_policy=GatePolicy.EQUIVALENCE_ONLY,
    ),
    Benchmark(
        name="dpi_watchdog_timeout",
        label="DPI watchdog timeout",
        category=Category.INTEGRATION,
        adapter_kind=AdapterKind.DPI_WATCHDOG_TIMEOUT,
        build_targets=(
            "cpp-dpi-watchdog-timeout-build",
            "cpp-dpi-watchdog-timeout-sv-build",
        ),
        binaries=(
            Binary(
                "cpp_dpi",
                "build/cpptb/watchdog_timeout/obj/Vdpi_stalling_responder",
            ),
            Binary(
                "pure_sv",
                "build/cpptb/watchdog_timeout/systemverilog_obj/Vstalling_responder_sv_tb",
            ),
        ),
        runner=Runner(
            commands=(
                ("make", "cpp-dpi-watchdog-timeout-run"),
                ("make", "cpp-dpi-watchdog-timeout-sv-run"),
            ),
        ),
        default_iterations=1,
        gate_policy=GatePolicy.EQUIVALENCE_ONLY,
    ),
    Benchmark(
        name="dpi_fault_injection",
        label="DPI fault injection",
        category=Category.INTEGRATION,
        adapter_kind=AdapterKind.DPI_FAULT_INJECTION,
        build_targets=(
            "cpp-dpi-fault-injection-build",
            "cpp-dpi-fault-injection-sv-build",
        ),
        binaries=(
            Binary(
                "cpp_dpi",
                "build/cpptb/fault_injection/obj/Vdpi_fault_injection",
            ),
            Binary(
                "pure_sv",
                "build/cpptb/fault_injection/systemverilog_obj/Vfault_injection_sv_tb",
            ),
        ),
        runner=Runner(
            commands=(
                ("make", "cpp-dpi-fault-injection-run"),
                ("make", "cpp-dpi-fault-injection-sv-run"),
            ),
        ),
        default_iterations=1,
        gate_policy=GatePolicy.EQUIVALENCE_ONLY,
    ),
    Benchmark(
        name="dpi_rich_data",
        label="DPI rich data",
        category=Category.INTEGRATION,
        adapter_kind=AdapterKind.DPI_RICH_DATA,
        build_targets=(
            "cpp-dpi-rich-data-build",
            "cpp-dpi-rich-data-sv-build",
        ),
        binaries=(
            Binary("cpp_dpi", "build/cpptb/rich_data/obj/Vdpi_rich_data"),
            Binary(
                "pure_sv",
                "build/cpptb/rich_data/systemverilog_obj/Vrich_data_sv_tb",
            ),
        ),
        runner=Runner(
            commands=(
                ("make", "cpp-dpi-rich-data-run"),
                ("make", "cpp-dpi-rich-data-sv-run"),
            ),
        ),
        default_iterations=1,
        gate_policy=GatePolicy.EQUIVALENCE_ONLY,
    ),
    Benchmark(
        name="dpi_interfaces",
        label="DPI interfaces and inouts",
        category=Category.INTEGRATION,
        adapter_kind=AdapterKind.DPI_INTERFACES,
        build_targets=(
            "cpp-dpi-interfaces-build",
            "cpp-dpi-interfaces-sv-build",
        ),
        binaries=(
            Binary(
                "cpp_dpi",
                "build/cpptb/interfaces/obj/Vdpi_stream_interfaces",
            ),
            Binary(
                "pure_sv",
                "build/cpptb/interfaces/systemverilog_obj/"
                "Vstream_interfaces_sv_tb",
            ),
        ),
        runner=Runner(
            commands=(
                ("make", "cpp-dpi-interfaces-run"),
                ("make", "cpp-dpi-interfaces-sv-run"),
            ),
        ),
        default_iterations=1,
        gate_policy=GatePolicy.EQUIVALENCE_ONLY,
    ),
    Benchmark(
        name="peripheral_suite",
        label="Peripheral suite",
        category=Category.INTEGRATION,
        adapter_kind=AdapterKind.PERIPHERAL_SUITE,
        build_targets=(
            "peripheral-suite-build",
            "peripheral-suite-dpi-build",
            "peripheral-suite-sv-build",
        ),
        binaries=(
            Binary("cpp_vpi", "build/benchmarks/peripheral_suite/peripheral_suite_host"),
            Binary(
                "cpp_dpi",
                "build/benchmarks/peripheral_suite/cpp_dpi_obj/Vdpi_peripheral_suite",
            ),
            Binary(
                "pure_sv",
                "build/benchmarks/peripheral_suite/pure_sv_obj/Vperipheral_suite_sv_tb",
            ),
        ),
        runner=Runner(
            commands=(("python3", "benchmarks/peripheral_suite/run_benchmark.py"),),
            iterations_argument="--iters",
            iterations_environment="PERIPHERAL_SUITE_ITERS",
            semantic_build_targets=(
                "peripheral-suite-dpi-build",
                "peripheral-suite-sv-build",
            ),
        ),
        default_iterations=10_000,
        gate_policy=GatePolicy.DIAGNOSTIC,
    ),
)

# A concise alias for callers that naturally refer to registry entries.
ENTRIES = BENCHMARKS
_BY_NAME = {benchmark.name: benchmark for benchmark in BENCHMARKS}


def get_benchmark(name: str) -> Benchmark:
    """Return the exact named benchmark, raising KeyError for unknown names."""
    return _BY_NAME[name]


def list_benchmarks(
    *,
    category: Category | str | None = None,
    gate_policy: GatePolicy | str | None = None,
) -> tuple[Benchmark, ...]:
    """List entries in stable registry order, optionally filtered."""
    wanted_category = Category(category) if category is not None else None
    wanted_policy = GatePolicy(gate_policy) if gate_policy is not None else None
    return tuple(
        benchmark
        for benchmark in BENCHMARKS
        if (wanted_category is None or benchmark.category is wanted_category)
        and (wanted_policy is None or benchmark.gate_policy is wanted_policy)
    )


def _makefile_contract(text: str) -> tuple[tuple[str, ...], dict[str, int]]:
    kernel_match = re.search(
        r"^AUTHORING_CORE_KERNELS\s*:=\s*(?P<kernels>[^\n#]+)", text, re.MULTILINE
    )
    kernels = tuple(kernel_match.group("kernels").split()) if kernel_match else ()
    templates = {
        name: int(template_id)
        for name, template_id in re.findall(
            r"^\$\(eval\s+\$\(call\s+AUTHORING_CORE_DPI_template\s*,\s*"
            r"([a-z0-9_]+)\s*,\s*([0-9]+)\s*\)\s*\)\s*$",
            text,
            re.MULTILINE,
        )
    }
    return kernels, templates


def consistency_errors(
    *,
    workload_kernels: Iterable[str] | None = None,
    makefile_path: str | Path = DEFAULT_MAKEFILE,
    makefile_text: str | None = None,
) -> tuple[str, ...]:
    """Return authoring registry drift errors without invoking external tools."""
    if workload_kernels is None:
        from benchmarks.authoring_core.workload import KERNELS

        workload_kernels = KERNELS

    authoring = list_benchmarks(category=Category.AUTHORING_FEATURE)
    registry_kernels = tuple(entry.name for entry in authoring)
    registry_templates = {entry.name: entry.template_id for entry in authoring}
    workload_kernels = tuple(workload_kernels)
    text = Path(makefile_path).read_text() if makefile_text is None else makefile_text
    makefile_kernels, makefile_templates = _makefile_contract(text)

    errors: list[str] = []
    for entry in BENCHMARKS:
        if entry.gate_policy is GatePolicy.WAIVED_HARD_1_10:
            if entry.waiver is None:
                errors.append(f"{entry.name} uses a waiver policy without waiver metadata")
            elif entry.waiver.max_ratio <= 1.10:
                errors.append(
                    f"{entry.name} waiver ceiling {entry.waiver.max_ratio} must exceed 1.10"
                )
            elif not entry.waiver.approved_on or not entry.waiver.rationale.strip():
                errors.append(f"{entry.name} waiver metadata is incomplete")
        elif entry.waiver is not None:
            errors.append(f"{entry.name} has waiver metadata without a waiver policy")
    if registry_kernels != workload_kernels:
        errors.append(
            f"registry authoring order {registry_kernels!r} != workload.KERNELS "
            f"{workload_kernels!r}"
        )
    if not makefile_kernels:
        errors.append("Makefile AUTHORING_CORE_KERNELS was not found")
    elif registry_kernels != makefile_kernels:
        errors.append(
            f"registry authoring order {registry_kernels!r} != Makefile "
            f"AUTHORING_CORE_KERNELS {makefile_kernels!r}"
        )
    if not makefile_templates:
        errors.append("Makefile authoring template IDs were not found")
    elif registry_templates != makefile_templates:
        errors.append(
            f"registry template IDs {registry_templates!r} != Makefile template IDs "
            f"{makefile_templates!r}"
        )
    return tuple(errors)


class RegistryConsistencyError(ValueError):
    pass


def check_consistency(**kwargs: object) -> None:
    """Raise RegistryConsistencyError when authoring metadata has drifted."""
    errors = consistency_errors(**kwargs)
    if errors:
        raise RegistryConsistencyError("; ".join(errors))


__all__ = [
    "AdapterKind",
    "BENCHMARKS",
    "ENTRIES",
    "Benchmark",
    "Binary",
    "Category",
    "GatePolicy",
    "PerformanceWaiver",
    "RegistryConsistencyError",
    "Runner",
    "check_consistency",
    "consistency_errors",
    "get_benchmark",
    "list_benchmarks",
]
