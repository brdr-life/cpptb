import hashlib
import importlib.util
import re
import subprocess
import sys
import unittest
from pathlib import Path


SUITE = Path(__file__).resolve().parents[1]


def load_equivalence_module():
    path = SUITE / "run_equivalence.py"
    spec = importlib.util.spec_from_file_location("aes_run_equivalence", path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def load_benchmark_module():
    path = SUITE / "run_benchmark.py"
    spec = importlib.util.spec_from_file_location("aes_run_benchmark", path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.path.insert(0, str(SUITE))
    try:
        spec.loader.exec_module(module)
    finally:
        sys.path.remove(str(SUITE))
    return module


class SecworksAesGroundTruthContractTests(unittest.TestCase):
    def test_upstream_oracle_is_the_pinned_source_file(self):
        digest = hashlib.sha256((SUITE / "upstream/tb_aes.v").read_bytes()).hexdigest()
        self.assertEqual(
            digest,
            "1d3931a3a9d0e3a94eed6711d2eb16871ce7e6753db23d0b5addd19999335bc5",
        )

    def test_oracle_covers_twenty_aes_128_and_aes_256_cases(self):
        source = (SUITE / "upstream/tb_aes.v").read_text(encoding="utf-8")
        calls = re.findall(r"^\s+ecb_mode_single_block_test\(", source, re.MULTILINE)
        self.assertEqual(len(calls), 20)
        self.assertIn("*** All %02d test cases completed successfully", source)
        self.assertIn("nist_aes128_key1", source)
        self.assertIn("nist_aes256_key1", source)

    def test_systemrdl_contract_covers_the_complete_public_register_map(self):
        source = (SUITE / "registers/aes.rdl").read_text(encoding="utf-8")
        for register in (
            "control",
            "status",
            "config",
            "key0",
            "key7",
            "block0",
            "block3",
            "result0",
            "result3",
        ):
            self.assertIn(f"}} {register} @", source)

    def test_cpptb_uses_generated_named_register_handles(self):
        source = (SUITE / "cpptb/testbench.cpp").read_text(encoding="utf-8")
        self.assertIn("secworks_aes_regs::RegModel<AesMaster>", source)
        self.assertIn("regs.control.write", source)
        self.assertIn("regs.config.write", source)
        self.assertNotIn("constexpr uint32_t kAddr", source)

    def test_passive_trace_has_no_benchmark_print_cost_by_default(self):
        source = (SUITE / "observers/aes_bus_observer.sv").read_text(
            encoding="utf-8"
        )
        self.assertIn('$test$plusargs("AES_BUS_TRACE")', source)
        self.assertIn("trace_enabled && reset_n && cs", source)

    def test_matched_sv_peer_has_the_same_twenty_vector_sequence(self):
        source = (SUITE / "systemverilog/repeated_aes_tb.sv").read_text(
            encoding="utf-8"
        )
        self.assertEqual(source.count("run_case(suite,"), 20)
        self.assertIn("#(100 * CLK_PERIOD)", source)
        self.assertIn("AES_REGMODEL_REPEATS", source)

    def test_result_parser_and_oracle_checksum(self):
        module = load_equivalence_module()
        result = module.parse_result(
            "SV_AES_REGMODEL_RESULT suites=1 cases=20 checks=80 "
            "checksum=46264475 failures=0"
        )
        self.assertEqual(result["checksum"], module.EXPECTED_CHECKSUM)
        trace = [
            "AES_BUS cycle=1 op=R address=30 data=00000001",
            "AES_BUS cycle=2 op=W address=08 data=00000002",
        ]
        checksum = ((0x811C9DC5 ^ 1) * 0x01000193) & 0xFFFFFFFF
        # Exercise the same fold independently without requiring a full build.
        observed = 0x811C9DC5
        for line in trace:
            if " op=R " in line:
                observed = (
                    (observed ^ int(line.rsplit("data=", 1)[1], 16))
                    * 0x01000193
                ) & 0xFFFFFFFF
        self.assertEqual(observed, checksum)

    def test_benchmark_normalizes_load_and_rejects_busy_samples(self):
        module = load_benchmark_module()
        quiet = module.load_snapshot((1.6, 1.0, 0.8), 8)
        busy = module.load_snapshot((2.8, 1.0, 0.8), 8)
        self.assertEqual(quiet["normalized_load_1m"], 0.2)
        self.assertEqual(busy["normalized_load_1m"], 0.35)
        assessment = module.assess_load([quiet, busy], 0.30)
        self.assertEqual(assessment["status"], "fail")
        self.assertEqual(assessment["maximum_normalized_load_1m"], 0.35)

    def test_benchmark_requires_balanced_process_order(self):
        completed = subprocess.run(
            [
                sys.executable,
                str(SUITE / "run_benchmark.py"),
                "--runs",
                "5",
                "--skip-build",
            ],
            cwd=SUITE,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(completed.returncode, 2)
        self.assertIn("--runs must be an even number", completed.stderr)

    def test_generated_header_tracks_codegen_and_environment_inputs(self):
        source = (SUITE / "Makefile").read_text(encoding="utf-8")
        self.assertIn("REGISTER_CODEGEN_SOURCES", source)
        self.assertIn("tools/codegen/cpptb_codegen/*.py", source)
        self.assertIn("$(ROOT)/pyproject.toml", source)
        self.assertIn("$(ROOT)/uv.lock", source)
        self.assertIn(
            "$(GENERATED_HEADER): $(RDL) $(REGISTER_CODEGEN_SOURCES)", source
        )


if __name__ == "__main__":
    unittest.main()
