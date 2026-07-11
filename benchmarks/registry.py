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
    DPI_MULTICLOCK = "dpi_multiclock"
    PERIPHERAL_SUITE = "peripheral_suite"


class GatePolicy(str, Enum):
    HARD_1_10 = "hard_1_10"
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
    "channel": 6,
    "all": 7,
}


def _authoring(name: str, label: str) -> Benchmark:
    dpi_binary = f"build/benchmarks/authoring_core/cpp_dpi_{name}/Vdpi_authoring_core"
    return Benchmark(
        name=name,
        label=label,
        category=Category.AUTHORING_FEATURE,
        adapter_kind=AdapterKind.AUTHORING_CORE,
        build_targets=(dpi_binary, "authoring-core-sv-build"),
        binaries=(
            Binary(
                "cpp_dpi",
                dpi_binary,
            ),
            Binary(
                "pure_sv",
                "build/benchmarks/authoring_core/pure_sv_obj/Vauthoring_core_sv_tb",
            ),
        ),
        runner=Runner(
            commands=(("python3", "benchmarks/authoring_core/run_benchmark.py"),),
            iterations_argument="--iters",
            iterations_environment="AUTHORING_CORE_ITERS",
            kernel_argument="--example",
        ),
        default_iterations=100_000,
        gate_policy=GatePolicy.HARD_1_10,
        template_id=_AUTHORING_TEMPLATE_IDS[name],
    )


BENCHMARKS: tuple[Benchmark, ...] = (
    _authoring("control", "Control"),
    _authoring("task_value", "Task value"),
    _authoring("clock_cycles", "Clock cycles"),
    _authoring("timeout", "Timeout"),
    _authoring("task_timeout", "Task timeout"),
    _authoring("wait_until", "Wait until"),
    _authoring("event", "Event"),
    _authoring("channel", "Channel"),
    _authoring("all", "All authoring features"),
    Benchmark(
        name="dpi_multiclock",
        label="DPI multiclock",
        category=Category.INTEGRATION,
        adapter_kind=AdapterKind.DPI_MULTICLOCK,
        build_targets=("cpp-dpi-multiclock-build", "cpp-dpi-multiclock-sv-build"),
        binaries=(
            Binary("cpp_dpi", "build/cpptb/dpi_multiclock_obj/Vdpi_dual_clock_mailbox"),
            Binary(
                "pure_sv",
                "build/cpptb/dpi_multiclock_sv_obj/Vdual_clock_mailbox_sv_tb",
            ),
        ),
        runner=Runner(
            commands=(
                ("make", "cpp-dpi-multiclock-run"),
                ("make", "cpp-dpi-multiclock-sv-run"),
            ),
            iterations_environment="CPPTB_MULTICLOCK_ITERS",
        ),
        default_iterations=16,
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
    "RegistryConsistencyError",
    "Runner",
    "check_consistency",
    "consistency_errors",
    "get_benchmark",
    "list_benchmarks",
]
