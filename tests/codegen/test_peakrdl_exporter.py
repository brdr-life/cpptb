from __future__ import annotations

import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

from cpptb_codegen.generate_dpi_bindings import generate_sources


REPO = Path(__file__).resolve().parents[2]
FIXTURE = REPO / "tests/codegen/fixtures/register_model.rdl"


class PeakRdlExporterTests(unittest.TestCase):
    def setUp(self) -> None:
        if shutil.which("peakrdl") is None:
            self.skipTest("PeakRDL optional dependencies are not installed")
        self.verilator_include: Path | None = None
        if shutil.which("verilator") is not None:
            result = subprocess.run(
                ["verilator", "--getenv", "VERILATOR_ROOT"],
                cwd=REPO,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            if result.returncode == 0:
                self.verilator_include = Path(result.stdout.strip()) / "include"

    def run_peakrdl(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["peakrdl", *args],
            cwd=REPO,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )

    def test_help_documents_naming_controls(self) -> None:
        result = self.run_peakrdl("cpptb", "--help")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("--rename INST_NAME", result.stdout)
        self.assertIn("-o OUTPUT", result.stdout)
        self.assertIn("--namespace NAMESPACE", result.stdout)
        self.assertIn("C++ namespace only", result.stdout)
        self.assertIn("--class-name CLASS_NAME", result.stdout)
        self.assertIn("--register-endianness {little,big}", result.stdout)
        self.assertIn("generated C++ register-model class name", result.stdout)

    def test_top_rename_is_distinct_from_cpp_names(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            renamed = root / "renamed.hpp"
            result = self.run_peakrdl(
                "cpptb",
                str(FIXTURE),
                "--rename",
                "renamed_map",
                "-o",
                str(renamed),
            )
            self.assertEqual(result.returncode, 0, result.stdout)
            renamed_source = renamed.read_text(encoding="utf-8")
            self.assertIn("namespace renamed_map", renamed_source)
            self.assertIn('.path = "renamed_map.control"', renamed_source)
            self.assertIn("class RegModel", renamed_source)

            custom = root / "product_aes.hpp"
            result = self.run_peakrdl(
                "cpptb",
                str(FIXTURE),
                "-o",
                str(custom),
                "--namespace",
                "product_regs",
                "--class-name",
                "ProductRegisters",
                "--register-endianness",
                "big",
            )
            self.assertEqual(result.returncode, 0, result.stdout)
            custom_source = custom.read_text(encoding="utf-8")
            self.assertIn("namespace product_regs", custom_source)
            self.assertIn("class ProductRegisters", custom_source)
            self.assertIn(
                ".endianness = cpptb::vc::RegisterEndianness::Big",
                custom_source,
            )
            self.assertIn('.path = "demo_regs.control"', custom_source)

    def test_user_effects_generate_and_execute_authored_policy(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "user_effects.rdl"
            output = root / "user_effects.hpp"
            source.write_text(
                textwrap.dedent(
                    """
                    addrmap user_effects {
                        external reg {
                            field {
                                sw = rw;
                                hw = r;
                                onread = ruser;
                                onwrite = wuser;
                            } custom[3:0];
                        } control @ 0x20;
                    };
                    """
                ),
                encoding="utf-8",
            )
            exported = self.run_peakrdl("cpptb", str(source), "-o", str(output))
            self.assertEqual(exported.returncode, 0, exported.stdout)
            generated = output.read_text(encoding="utf-8")
            self.assertIn("RegisterReadEffect::User", generated)
            self.assertIn("RegisterWriteEffect::User", generated)
            self.assertIn("RegisterUserEffectPolicy& user_effects", generated)
            self.assertIn("backdoor, user_effects", generated)

            if self.verilator_include is None or shutil.which("c++") is None:
                return
            runtime = root / "user_effects_runtime.cpp"
            runtime.write_text(
                textwrap.dedent(
                    """
                    #include "user_effects.hpp"

                    struct Master {
                        using address_type = uint32_t;
                        using data_type = uint32_t;
                        using byte_enable_type = uint8_t;
                        using write_request_type = cpptb::vc::MemoryWriteRequest<
                            address_type, data_type, byte_enable_type>;
                        using read_request_type =
                            cpptb::vc::MemoryReadRequest<address_type>;
                        using write_response_type = cpptb::vc::MemoryWriteResponse;
                        using read_response_type =
                            cpptb::vc::MemoryReadResponse<data_type>;

                        cpptb::coro::Task<write_response_type> write(
                            write_request_type request) {
                            address = request.address;
                            storage = request.data;
                            co_return write_response_type{};
                        }
                        cpptb::coro::Task<read_response_type> read(
                            read_request_type) {
                            co_return read_response_type{.data = storage};
                        }
                        uint32_t address = 0;
                        uint32_t storage = 0;
                    };

                    struct Policy final : cpptb::vc::RegisterUserEffectPolicy {
                        bool encode_write(
                            const cpptb::vc::RegisterUserEffectBitContext& c)
                            override {
                            return c.previous_valid ? c.previous != c.value
                                                    : c.value;
                        }
                        cpptb::vc::RegisterUserEffectBitResult predict_write(
                            const cpptb::vc::RegisterUserEffectBitContext& c)
                            override {
                            last_path = c.field_descriptor.path;
                            return {.value = c.previous != c.value,
                                    .valid = c.previous_valid};
                        }
                        cpptb::vc::RegisterUserEffectBitResult predict_read(
                            const cpptb::vc::RegisterUserEffectBitContext& c)
                            override {
                            return {.value = !c.value, .valid = true};
                        }
                        std::string_view last_path;
                    };

                    cpptb::coro::Task<void> exercise(
                        user_effects::RegModel<Master>& model,
                        bool& passed) {
                        model.control.predict(
                            0, cpptb::vc::RegisterPrediction::Direct);
                        model.control.custom.set_desired(0xa);
                        const auto response = co_await model.control.update();
                        passed = response.okay() &&
                                 model.control.mirrored() == 0xa;
                    }

                    int main() {
                        cpptb::coro::Testbench scheduler;
                        cpptb::TestResult result;
                        cpptb::TestContext test{scheduler, result};
                        Master master;
                        Policy policy;
                        user_effects::RegModel model{test, master, policy};
                        bool passed = false;
                        scheduler.spawn_detached(exercise(model, passed));
                        return scheduler.done() && passed &&
                                       master.address == 0x20 &&
                                       master.storage == 0xa &&
                                       policy.last_path ==
                                           "user_effects.control.custom"
                                   ? 0
                                   : 1;
                    }
                    """
                ),
                encoding="utf-8",
            )
            executable = root / "user_effects_runtime"
            compiled = subprocess.run(
                [
                    "c++",
                    "-std=c++20",
                    f"-I{REPO / 'include'}",
                    f"-I{self.verilator_include}",
                    f"-I{self.verilator_include / 'vltstd'}",
                    f"-I{root}",
                    str(runtime),
                    "-o",
                    str(executable),
                ],
                cwd=REPO,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            executed = subprocess.run(
                [str(executable)],
                cwd=REPO,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)

    def test_systemrdl_generates_typed_cpp_model(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "demo_regs.hpp"
            result = self.run_peakrdl(
                "cpptb",
                str(FIXTURE),
                "-o",
                str(output),
                "--namespace",
                "generated_demo",
            )
            self.assertEqual(result.returncode, 0, result.stdout)
            generated = output.read_text(encoding="utf-8")
            self.assertTrue(
                generated.startswith("// Generated by cpptb-codegen 0.1.0.")
            )
            self.assertIn("namespace generated_demo", generated)
            self.assertIn("enum class mode_e : uint64_t", generated)
            self.assertIn("mode_e::ACTIVE", generated)
            self.assertIn("cpptb_diagnostic_name(mode_e value)", generated)
            self.assertIn("RegisterEnumFieldHandle<mode_e", generated)
            self.assertNotIn("using namespace cpptb::vc", generated)
            self.assertIn(
                "inline constexpr cpptb::vc::RegisterBlockDescriptor descriptor",
                generated,
            )
            self.assertIn('.path = "demo_regs.control"', generated)
            self.assertIn('.address = UINT64_C(0x4)', generated)
            self.assertIn(
                ".write_effect = cpptb::vc::RegisterWriteEffect::WriteOneClear",
                generated,
            )
            self.assertIn(
                ".read_effect = cpptb::vc::RegisterReadEffect::Clear", generated
            )
            self.assertIn('.volatile_value = true', generated)
            self.assertIn('.volatile_value = false', generated)
            self.assertIn('Register3Handle<Master> security_key;', generated)
            self.assertIn('RegisterScopeView_security<Master> security;', generated)
            self.assertIn('RegisterViewArray<Register4Handle<Master>&, Register5Handle<Master>&> lane_control;', generated)
            self.assertIn('RegisterScopeView_bank_0<Master>', generated)
            self.assertIn('for_each_register(Function&& function)', generated)
            self.assertIn('for_each_register_async(', generated)
            self.assertIn('for_each_field(Function&& function)', generated)
            self.assertIn('for_each_memory(Function&& function)', generated)
            self.assertIn(
                'const cpptb::vc::RegisterBlockDescriptor& descriptor()',
                generated,
            )
            self.assertIn('void reset_all()', generated)
            self.assertIn('void set_auto_predict(bool enabled)', generated)
            self.assertIn('cpptb::coro::Task<void> update_all()', generated)
            self.assertIn('cpptb::coro::Task<void> mirror_all(', generated)
            self.assertIn(
                'cpptb::vc::RegisterFieldHandle<Master> pending;', generated
            )
            self.assertIn(
                'cpptb::vc::RegisterMemoryHandle<Master> buffer;', generated
            )
            self.assertIn('.entries = UINT64_C(16)', generated)
            self.assertIn('.hdl_path = "u_regs.buffer_storage"', generated)
            self.assertIn('.path = "demo_regs.split_access"', generated)
            self.assertIn('.access_width = 16', generated)
            self.assertIn('register_handles() noexcept', generated)
            self.assertIn('.path = "u_regs.control_q[31:0]"', generated)
            self.assertIn('.path = "u_regs.pending_storage[15:8]"', generated)
            self.assertIn('.path = "u_regs.sampled_bits[7]"', generated)
            self.assertIn('cpptb_signal<"u_regs.buffer_storage">()', generated)
            self.assertIn('.register_lsb = 8', generated)
            self.assertIn('class DutBackdoor final', generated)
            self.assertIn('RegisterMemoryBackdoor<', generated)
            self.assertIn('void peek_into(', generated)
            self.assertIn('std::span<const memory_data_type> values', generated)
            self.assertIn('make_backdoor(Dut dut)', generated)

            compile_test = Path(directory) / "compile.cpp"
            compile_test.write_text(
                '#include "demo_regs.hpp"\n'
                'static_assert(generated_demo::registers.size() == 8);\n'
                'static_assert(generated_demo::memories.size() == 1);\n'
                'static_assert(generated_demo::registers[1].fields[0].reset_value == 0xff);\n'
                'static_assert(generated_demo::registers[1].reset_mask == 0xff);\n'
                'static_assert(generated_demo::registers[2].access_width == 16);\n'
                'static_assert(generated_demo::registers[0].backdoor_slices.size() == 1);\n'
                'static_assert(generated_demo::registers[1].backdoor_slices.size() == 9);\n'
                'static_assert(generated_demo::registers[0].fields[0].backdoor_slices.size() == 1);\n'
                'static_assert(generated_demo::memories[0].hdl_path == "u_regs.buffer_storage");\n'
                'int main() {}\n',
                encoding="utf-8",
            )
            if self.verilator_include is None or shutil.which("c++") is None:
                return
            compiled = subprocess.run(
                [
                    "c++",
                    "-std=c++20",
                    f"-I{REPO / 'include'}",
                    f"-I{self.verilator_include}",
                    f"-I{self.verilator_include / 'vltstd'}",
                    f"-I{directory}",
                    "-fsyntax-only",
                    str(compile_test),
                ],
                cwd=REPO,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)

            runtime_test = Path(directory) / "runtime.cpp"
            runtime_test.write_text(
                textwrap.dedent(
                    """
                    #include <cstdint>
                    #include <cstdio>
                    #include <vector>
                    #include "cpptb/hierarchy.hpp"
                    #include "cpptb_vc/register_sequences.hpp"
                    #include "demo_regs.hpp"

                    template <size_t Width>
                    struct FakeSignal {
                        using value_type = uint64_t;
                        using raw_value_type = uint64_t;
                        static constexpr size_t width = Width;
                        uint64_t* value = nullptr;

                        uint64_t get() const { return *value; }
                        void deposit(uint64_t next) const {
                            *value = next & cpptb::vc::register_mask(Width);
                        }
                    };

                    template <size_t Width, int32_t Low, size_t Size>
                    struct FakeMemory {
                        using value_type = uint64_t;
                        using raw_value_type = uint64_t;
                        static constexpr size_t width = Width;
                        static constexpr int32_t low = Low;
                        static constexpr size_t size = Size;

                        struct Element {
                            uint64_t* value = nullptr;
                            uint64_t get() const { return *value; }
                            void deposit(uint64_t next) const {
                                *value = next & cpptb::vc::register_mask(Width);
                            }
                        };

                        Element at(int32_t index) const {
                            return Element{values + index - Low};
                        }

                        uint64_t* values = nullptr;
                    };

                    struct FakeDut {
                        uint64_t* control = nullptr;
                        uint64_t* pending_storage = nullptr;
                        uint64_t* sampled_bits = nullptr;
                        uint64_t* key = nullptr;
                        uint64_t* buffer = nullptr;

                        template <cpptb::hierarchy::FixedString Path>
                        auto cpptb_signal() const {
                            if constexpr (Path.view() == "u_regs.control_q") {
                                return FakeSignal<32>{control};
                            } else if constexpr (
                                Path.view() == "u_regs.pending_storage") {
                                return FakeSignal<16>{pending_storage};
                            } else if constexpr (
                                Path.view() == "u_regs.sampled_bits") {
                                return FakeSignal<8>{sampled_bits};
                            } else if constexpr (
                                Path.view() == "u_regs.security.key_q") {
                                return FakeSignal<32>{key};
                            } else if constexpr (
                                Path.view() == "u_regs.buffer_storage") {
                                return FakeMemory<32, 8, 16>{buffer};
                            } else {
                                static_assert(Path.view().empty(),
                                              "unexpected fake HDL path");
                            }
                        }
                    };

                    struct FakeMaster {
                        using address_type = uint32_t;
                        using data_type = uint32_t;
                        using byte_enable_type = uint8_t;
                        using write_request_type = cpptb::vc::MemoryWriteRequest<
                            address_type, data_type, byte_enable_type>;
                        using read_request_type =
                            cpptb::vc::MemoryReadRequest<address_type>;
                        using write_response_type = cpptb::vc::MemoryWriteResponse;
                        using read_response_type =
                            cpptb::vc::MemoryReadResponse<data_type>;

                        cpptb::coro::Task<write_response_type> write(
                            write_request_type request) {
                            writes.push_back(request);
                            co_return write_response_type{};
                        }
                        cpptb::coro::Task<read_response_type> read(
                            read_request_type request) {
                            const uint32_t data =
                                request.address == 0 ? 7u :
                                request.address == 4 ? 0xffu : 0u;
                            co_return read_response_type{.data = data};
                        }
                        std::vector<write_request_type> writes;
                    };

                    cpptb::coro::Task<void> exercise(
                        generated_demo::RegModel<FakeMaster>& model,
                        FakeMaster& master, cpptb::TestContext& test,
                        bool& passed) {
                        const auto reset =
                            co_await cpptb::vc::register_reset_check(
                                test, model);
                        if (reset.registers_visited != 8 ||
                            reset.registers_tested != 1) {
                            std::fprintf(stderr,
                                "reset visited=%llu tested=%llu skipped=%llu\\n",
                                static_cast<unsigned long long>(
                                    reset.registers_visited),
                                static_cast<unsigned long long>(
                                    reset.registers_tested),
                                static_cast<unsigned long long>(
                                    reset.registers_skipped));
                        }
                        model.reset_all();
                        model.control.enable.set_desired(0);
                        const auto response = co_await model.control.update();
                        cpptb::vc::RegisterAddressMap debug_map{
                            "debug", master, 0x8000};
                        debug_map.route(model.control.descriptor(), 0x40)
                            .route(model.buffer.descriptor(), 0x200);
                        const auto mapped_register_write =
                            co_await model.control.write(0x5u, debug_map);
                        const std::array<uint32_t, 2> mapped_memory_values{
                            0xabcdu, 0x1234u};
                        const auto mapped_memory_write =
                            co_await model.buffer.write(
                                2,
                                std::span<const uint32_t>{
                                    mapped_memory_values},
                                debug_map);
                        const auto memory_write = co_await model.buffer.write(
                            3, 0xcafe1234u, cpptb::vc::AccessPath::Backdoor);
                        const auto memory_read = co_await model.buffer.read(
                            3, cpptb::vc::AccessPath::Backdoor);
                        model.buffer.poke(4, 0x55aa9876u);
                        const std::array<uint32_t, 3> bulk_values{
                            0x11u, 0x22u, 0x33u};
                        std::array<uint32_t, 3> bulk_readback{};
                        const auto bulk_write = co_await model.buffer.write(
                            5, std::span<const uint32_t>{bulk_values},
                            cpptb::vc::AccessPath::Backdoor);
                        const auto bulk_read = co_await model.buffer.read_into(
                            5, std::span<uint32_t>{bulk_readback},
                            cpptb::vc::AccessPath::Backdoor);
                        passed = reset.registers_visited == 8 &&
                                 reset.registers_tested == 1 &&
                                 response.okay() && memory_write.okay() &&
                                 memory_read.okay() &&
                                 memory_read.data == 0xcafe1234u &&
                                 model.buffer.peek(4) == 0x55aa9876u &&
                                 bulk_write.transfers_completed == 3 &&
                                 bulk_read.transfers_completed == 3 &&
                                 bulk_readback == bulk_values &&
                                 mapped_register_write.okay() &&
                                 mapped_memory_write.okay() &&
                                 master.writes.size() == 4 &&
                                 master.writes[0].address == 0 &&
                                 master.writes[0].data == 6 &&
                                 master.writes[1].address == 0x8040 &&
                                 master.writes[2].address == 0x8208 &&
                                 master.writes[3].address == 0x820c &&
                                 model.control.mirrored_valid_mask() == 0xf &&
                                 model.status.pending.desired_valid_mask() == 0xff &&
                                 model.status.sampled.desired_valid_mask() == 0;
                    }

                    int main() {
                        cpptb::coro::Testbench scheduler;
                        cpptb::TestResult result;
                        cpptb::TestContext test{scheduler, result};
                        FakeMaster master;
                        uint64_t control = 0x12345678;
                        uint64_t pending_storage = 0xa500;
                        uint64_t sampled_bits = 0x3c;
                        uint64_t key = 0;
                        std::array<uint64_t, 16> buffer{};
                        auto backdoor =
                            generated_demo::make_backdoor<FakeMaster>(FakeDut{
                                &control, &pending_storage, &sampled_bits, &key,
                                buffer.data()});
                        generated_demo::RegModel model{
                            test, master, 0, &backdoor};
                        model.reset_all();
                        model.control.mode.set_desired(
                            generated_demo::mode_e::ACTIVE);
                        if (model.control.mode.desired() !=
                                generated_demo::mode_e::ACTIVE ||
                            generated_demo::cpptb_diagnostic_name(
                                model.control.mode.desired()) !=
                                "mode_e::ACTIVE") {
                            return 1;
                        }
                        model.control.mode.raw().set_desired(5);
                        if (static_cast<uint64_t>(
                                model.control.mode.desired()) != 5) {
                            return 1;
                        }
                        model.control.mode.set_desired(
                            generated_demo::mode_e::ACTIVE);
                        model.security.key.key.set_desired(0x1234u);
                        model.lane_control.template at<1>().value.set_desired(0x5au);
                        model.bank.template at<0>().control.value.set_desired(0xa55au);
                        size_t visited_registers = 0;
                        model.for_each_register(
                            [&](auto&) { ++visited_registers; });
                        size_t visited_fields = 0;
                        model.control.for_each_field(
                            [&](auto&) { ++visited_fields; });
                        size_t visited_memories = 0;
                        model.for_each_memory(
                            [&](auto&) { ++visited_memories; });
                        size_t visited_lane_slice = 0;
                        model.lane_control.template for_each_slice<1, 1>(
                            [&](auto&) { ++visited_lane_slice; });
                        if (visited_registers != 8 || visited_fields != 2 ||
                            visited_memories != 1 || visited_lane_slice != 1 ||
                            model.descriptor().registers.size() != 8 ||
                            model.control.path() != "demo_regs.control" ||
                            model.control.mode.path() !=
                                "demo_regs.control.mode" ||
                            model.control.hdl_path() !=
                                "u_regs.control_q[31:0]" ||
                            model.buffer.hdl_path() !=
                                "u_regs.buffer_storage" ||
                            model.lane_control.template at<1>().address() != 0x44u ||
                            model.bank.template at<1>().control.address() != 0x70u) {
                            return 1;
                        }
                        cpptb::vc::RegisterPredictor predictor{
                            test, model.register_handles()};
                        predictor.write(cpptb::vc::MemoryTransaction<
                            uint32_t, uint32_t, uint8_t>{
                                .operation = cpptb::vc::MemoryOperation::Write,
                                .address = 4,
                                .data = 2,
                                .byte_enable = 0x1,
                            });
                        bool passed = false;
                        scheduler.spawn_detached(
                            exercise(model, master, test, passed));
                        backdoor.poke(generated_demo::registers[0], 0,
                                      0x12345678u);
                        const bool direct_path =
                            backdoor.peek(generated_demo::registers[0], 0) ==
                            0x12345678u;
                        bool missing_path_diagnostic = false;
                        try {
                            (void)backdoor.peek(generated_demo::registers[2], 8);
                        } catch (const std::logic_error& error) {
                            missing_path_diagnostic =
                                std::string{error.what()}.find(
                                    "demo_regs.split_access") !=
                                std::string::npos;
                        }
                        model.status.poke(0x5a96u);
                        const bool sliced_path =
                            pending_storage == 0x9600u &&
                            sampled_bits == 0x5au &&
                            model.status.peek() == 0x5a96u;
                        const bool done = scheduler.done();
                        const bool predicted = predictor.writes() == 1;
                        if (!done || !passed || !direct_path || !sliced_path ||
                            !missing_path_diagnostic || !predicted) {
                            std::fprintf(stderr,
                                "done=%d passed=%d direct=%d sliced=%d "
                                "predicted=%d "
                                "writes=%zu data=%u\\n",
                                done, passed, direct_path, sliced_path,
                                predicted,
                                master.writes.size(),
                                master.writes.empty() ? 0u :
                                    master.writes[0].data);
                            return 1;
                        }
                        return 0;
                    }
                    """
                ),
                encoding="utf-8",
            )
            runtime_executable = Path(directory) / "runtime"
            runtime_compiled = subprocess.run(
                [
                    "c++",
                    "-std=c++20",
                    f"-I{REPO / 'include'}",
                    f"-I{self.verilator_include}",
                    f"-I{self.verilator_include / 'vltstd'}",
                    f"-I{directory}",
                    str(runtime_test),
                    "-o",
                    str(runtime_executable),
                ],
                cwd=REPO,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(runtime_compiled.returncode, 0, runtime_compiled.stdout)
            runtime = subprocess.run(
                [str(runtime_executable)],
                cwd=REPO,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(runtime.returncode, 0, runtime.stdout)

    def test_generated_backdoor_marks_only_typed_hierarchy_accesses(self) -> None:
        if self.verilator_include is None or shutil.which("c++") is None:
            self.skipTest("C++ and Verilator headers are required")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            generated = root / "generated"
            generate_sources(
                [REPO / "tests/codegen/fixtures/hierarchy_catalog.sv"],
                top="hierarchy_catalog",
                target="hierarchy_catalog",
                output_dir=generated,
                base_dir=REPO,
            )
            rdl = root / "hierarchy_regs.rdl"
            rdl.write_text(
                textwrap.dedent(
                    """
                    addrmap hierarchy_regs {
                        default regwidth = 8;
                        default accesswidth = 8;
                        reg {
                            hdl_path = "block1.storage";
                            field { sw = rw; hw = rw; } value[7:0];
                        } control @ 0x0;
                        external mem {
                            mementries = 4;
                            memwidth = 16;
                            sw = rw;
                            hdl_path_slice = '{"block1.memory"};
                        } ram @ 0x100;
                    };
                    """
                ),
                encoding="utf-8",
            )
            exported = self.run_peakrdl(
                "cpptb",
                str(rdl),
                "-o",
                str(generated / "hierarchy_regs.hpp"),
                "--namespace",
                "hierarchy_regs",
            )
            self.assertEqual(exported.returncode, 0, exported.stdout)

            discovery = root / "discover.cpp"
            discovery.write_text(
                textwrap.dedent(
                    """
                    #include <algorithm>
                    #include <cstdint>
                    #include "cpptb/cpptb.hpp"
                    #include "dut.hpp"
                    #include "hierarchy_regs.hpp"

                    struct Master {
                        using address_type = uint32_t;
                        using data_type = uint32_t;
                        using byte_enable_type = uint8_t;
                        using write_request_type = cpptb::vc::MemoryWriteRequest<
                            address_type, data_type, byte_enable_type>;
                        using read_request_type =
                            cpptb::vc::MemoryReadRequest<address_type>;
                        using write_response_type = cpptb::vc::MemoryWriteResponse;
                        using read_response_type =
                            cpptb::vc::MemoryReadResponse<data_type>;
                        cpptb::coro::Task<write_response_type> write(
                            write_request_type) { co_return write_response_type{}; }
                        cpptb::coro::Task<read_response_type> read(
                            read_request_type) { co_return read_response_type{}; }
                    };

                    int main() {
                        cpptb::coro::Testbench scheduler;
                        cpptb::TestResult result;
                        cpptb::TestContext test{scheduler, result};
                        Master master;
                        cpptb::Dut dut;
                        auto backdoor =
                            hierarchy_regs::make_backdoor<Master>(dut);
                        hierarchy_regs::RegModel model{
                            test, master, 0, &backdoor};
                        (void)model.control.peek();
                        model.control.poke(0x5a);
                        (void)model.ram.peek(0);
                        model.ram.poke(1, 0xbeef);

                        const auto plan =
                            cpptb::hierarchy::discovered_access_plan();
                        const auto has = [&](std::string_view path,
                                             cpptb::hierarchy::Operation op) {
                            return std::ranges::find(
                                       plan,
                                       cpptb::hierarchy::Access{
                                           path, op}) != plan.end();
                        };
                        return plan.size() == 4 &&
                                       has("block1.storage",
                                           cpptb::hierarchy::Operation::Get) &&
                                       has("block1.storage",
                                           cpptb::hierarchy::Operation::Deposit) &&
                                       has("block1.memory",
                                           cpptb::hierarchy::Operation::Get) &&
                                       has("block1.memory",
                                           cpptb::hierarchy::Operation::Deposit)
                                   ? 0
                                   : 1;
                    }
                    """
                ),
                encoding="utf-8",
            )
            executable = root / "discover"
            compiled = subprocess.run(
                [
                    "c++",
                    "-std=c++20",
                    "-DCPPTB_HIERARCHY_DISCOVERY",
                    f"-I{REPO / 'include'}",
                    f"-I{self.verilator_include}",
                    f"-I{self.verilator_include / 'vltstd'}",
                    f"-I{generated}",
                    str(discovery),
                    "-o",
                    str(executable),
                ],
                cwd=REPO,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            run = subprocess.run(
                [str(executable)],
                cwd=REPO,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(run.returncode, 0, run.stdout)

    def test_ambiguous_hdl_path_slices_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "ambiguous.rdl"
            source.write_text(
                textwrap.dedent(
                    """
                    addrmap ambiguous {
                        default regwidth = 8;
                        reg {
                            field {
                                sw = rw;
                                hdl_path_slice = '{"bit2", "bit1"};
                            } value[2:0];
                        } control @ 0;
                    };
                    """
                ),
                encoding="utf-8",
            )
            result = self.run_peakrdl(
                "cpptb", str(source), "-o", str(root / "ambiguous.hpp")
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("one path per bit from MSB to LSB", result.stdout)

    def test_systemrdl_register_aliases_are_rejected_explicitly(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "alias.rdl"
            source.write_text(
                textwrap.dedent(
                    """
                    reg control_t {
                        field { sw = rw; } value[7:0];
                    };
                    addrmap aliases {
                        control_t primary @ 0;
                        alias primary control_t alias_view @ 4;
                    };
                    """
                ),
                encoding="utf-8",
            )
            result = self.run_peakrdl(
                "cpptb", str(source), "-o", str(root / "alias.hpp")
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("SystemRDL register aliases are not yet supported", result.stdout)
            self.assertIn("RegisterAddressMap alias", result.stdout)

    def test_split_memory_backdoor_paths_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "split_memory.rdl"
            source.write_text(
                textwrap.dedent(
                    """
                    addrmap split_memory {
                        external mem {
                            mementries = 4;
                            memwidth = 32;
                            sw = rw;
                            hdl_path_slice = '{"low_words", "high_words"};
                        } ram @ 0;
                    };
                    """
                ),
                encoding="utf-8",
            )
            result = self.run_peakrdl(
                "cpptb", str(source), "-o", str(root / "split_memory.hpp")
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "memory hdl_path_slice must contain one path", result.stdout
            )

    def test_ipxact_import_uses_the_same_generated_contract(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            ipxact = Path(directory) / "demo_regs.xml"
            exported = self.run_peakrdl(
                "ip-xact", str(FIXTURE), "-o", str(ipxact)
            )
            self.assertEqual(exported.returncode, 0, exported.stdout)

            output = Path(directory) / "from_ipxact.hpp"
            imported = self.run_peakrdl(
                "cpptb", str(ipxact), "-o", str(output)
            )
            self.assertEqual(imported.returncode, 0, imported.stdout)
            generated = output.read_text(encoding="utf-8")
            self.assertIn('Register0Handle<Master> control;', generated)
            self.assertIn('.path = "demo_regs.control"', generated)
            # PeakRDL's IP-XACT exporter warns and drops nested SystemRDL mem
            # nodes. Memory import remains supported when the source IP-XACT
            # represents the memory as an address block.
            self.assertIn(
                'std::array<cpptb::vc::RegisterMemoryDescriptor, 0>', generated
            )

    def test_legitimate_single_child_addrmap_keeps_outer_hierarchy(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "nested.rdl"
            source.write_text(
                textwrap.dedent(
                    """
                    addrmap inner_block {
                        reg {
                            field { sw = rw; } value[31:0];
                        } control @ 0x0;
                    };
                    addrmap outer_block {
                        inner_block inner @ 0x100;
                    };
                    """
                ),
                encoding="utf-8",
            )
            output = Path(directory) / "nested.hpp"
            result = self.run_peakrdl(
                "cpptb", str(source), "-o", str(output), "--top", "outer_block"
            )
            self.assertEqual(result.returncode, 0, result.stdout)
            generated = output.read_text(encoding="utf-8")
            self.assertIn('.name = "outer_block"', generated)
            self.assertIn('.path = "outer_block.inner.control"', generated)
            self.assertIn('.address = UINT64_C(0x100)', generated)

    def test_wide_register_generates_typed_bits_frontdoor(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "wide.rdl"
            source.write_text(
                "addrmap wide_top { hdl_path = \"dut\"; "
                "reg { hdl_path = \"wide_q\"; regwidth = 128; accesswidth = 64; "
                "field { sw = rw; } value[127:0]; } wide @ 0x0; "
                "reg { field { sw = rw; } value[31:0]; } narrow @ 0x20; "
                "external mem { mementries = 2; memwidth = 128; sw = rw; "
                "hdl_path_slice = '{\"wide_memory\"}; } memory @ 0x40; };\n",
                encoding="utf-8",
            )
            output = Path(directory) / "wide.hpp"
            result = self.run_peakrdl("cpptb", str(source), "-o", str(output))
            self.assertEqual(result.returncode, 0, result.stdout)
            generated = output.read_text(encoding="utf-8")
            self.assertIn("WideRegisterHandle<128, Master>", generated)
            self.assertIn("WideRegisterFieldHandle<128, 128, Master>", generated)
            self.assertIn("register_0_reset_value_words", generated)
            self.assertIn("RegisterHandle<Master>", generated)
            self.assertIn("public cpptb::vc::WideRegisterBackdoor", generated)
            self.assertIn("public cpptb::vc::WideRegisterMemoryBackdoor", generated)
            self.assertIn("void peek_words(", generated)
            self.assertIn('cpptb_signal<"dut.wide_q">()', generated)
            self.assertIn("WideRegisterMemoryHandle<128, Master> memory", generated)
            self.assertIn('.access_width = 64', generated)
            self.assertNotIn("register_handles() noexcept", generated)

            if self.verilator_include is None or shutil.which("c++") is None:
                return
            runtime = Path(directory) / "wide_runtime.cpp"
            runtime.write_text(
                textwrap.dedent(
                    """
                    #include <cstdint>
                    #include "cpptb/hierarchy.hpp"
                    #include "wide.hpp"

                    template <std::size_t Width>
                    struct FakeSignal {
                        using value_type = cpptb::Bits<Width>;
                        static constexpr std::size_t width = Width;
                        value_type* value = nullptr;

                        value_type get() const { return *value; }
                        void deposit(value_type next) const { *value = next; }
                    };

                    struct FakeDut {
                        cpptb::Bits<128>* wide = nullptr;
                        cpptb::Bits<128>* memory = nullptr;

                        struct Memory {
                            using value_type = cpptb::Bits<128>;
                            static constexpr std::size_t width = 128;
                            static constexpr std::size_t size = 2;
                            static constexpr int32_t low = 0;

                            FakeSignal<128> at(int32_t index) const {
                                return FakeSignal<128>{values + index};
                            }

                            value_type* values = nullptr;
                        };

                        template <cpptb::hierarchy::FixedString Path>
                        auto cpptb_signal() const {
                            if constexpr (Path.view() == "dut.wide_q") {
                                return FakeSignal<128>{wide};
                            } else if constexpr (
                                Path.view() == "dut.wide_memory") {
                                return Memory{memory};
                            } else {
                                static_assert(Path.view().empty(),
                                              "unexpected fake HDL path");
                            }
                        }
                    };

                    struct Master {
                        using address_type = uint32_t;
                        using data_type = uint64_t;
                        using byte_enable_type = uint8_t;
                        using write_request_type = cpptb::vc::MemoryWriteRequest<
                            address_type, data_type, byte_enable_type>;
                        using read_request_type =
                            cpptb::vc::MemoryReadRequest<address_type>;
                        using write_response_type =
                            cpptb::vc::MemoryWriteResponse;
                        using read_response_type =
                            cpptb::vc::MemoryReadResponse<data_type>;
                        cpptb::coro::Task<write_response_type> write(
                            write_request_type) { co_return write_response_type{}; }
                        cpptb::coro::Task<read_response_type> read(
                            read_request_type) { co_return read_response_type{}; }
                    };

                    int main() {
                        cpptb::coro::Testbench scheduler;
                        cpptb::TestResult result;
                        cpptb::TestContext test{scheduler, result};
                        Master master;
                        auto storage = cpptb::Bits<128>::from_hex("0x1234");
                        std::array<cpptb::Bits<128>, 2> memory{};
                        auto backdoor =
                            wide_top::make_backdoor<Master>(
                                FakeDut{&storage, memory.data()});
                        wide_top::RegModel model{test, master, 0, &backdoor};
                        const auto value = cpptb::Bits<128>::from_hex(
                            "0x112233445566778899aabbccddeeff00");
                        model.wide.value.set_desired(value);
                        model.wide.poke(value);
                        model.memory.poke(1, value);
                        std::size_t visited = 0;
                        model.for_each_register([&](auto&) { ++visited; });
                        return model.wide.value.desired() == value &&
                                   model.wide.peek() == value &&
                                   model.memory.peek(1) == value &&
                                   memory[1] == value && storage == value &&
                                   visited == 2
                                   ? 0 : 1;
                    }
                    """
                ),
                encoding="utf-8",
            )
            executable = Path(directory) / "wide_runtime"
            compiled = subprocess.run(
                [
                    "c++",
                    "-std=c++20",
                    f"-I{REPO / 'include'}",
                    f"-I{self.verilator_include}",
                    f"-I{self.verilator_include / 'vltstd'}",
                    f"-I{directory}",
                    str(runtime),
                    "-o",
                    str(executable),
                ],
                cwd=REPO,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            executed = subprocess.run(
                [str(executable)],
                cwd=REPO,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout)

    def test_reset_reference_error_names_the_field(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "reset_reference.rdl"
            source.write_text(
                "signal { signalwidth = 1; } reset_source; "
                "addrmap reset_top { reg { field { sw = rw; "
                "reset = reset_source; } value[0:0]; } control @ 0x0; };\n",
                encoding="utf-8",
            )
            result = self.run_peakrdl(
                "cpptb",
                str(source),
                "-o",
                str(Path(directory) / "reset.hpp"),
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "reset_top.control.value: reset references are not supported",
                result.stdout,
            )


if __name__ == "__main__":
    unittest.main()
