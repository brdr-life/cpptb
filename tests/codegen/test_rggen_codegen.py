from __future__ import annotations

import json
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

from cpptb_codegen.rggen_codegen import (
    RgGenCodegenError,
    export_rggen_model,
    extract_rggen_block,
)


REPO = Path(__file__).resolve().parents[2]
FIXTURE = REPO / "tests/codegen/fixtures/rggen_registers.yml"


class RgGenCodegenTests(unittest.TestCase):
    def test_native_yaml_preserves_hierarchy_policies_and_memory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "rggen_registers.hpp"
            export_rggen_model(FIXTURE, output)
            generated = output.read_text(encoding="utf-8")
            self.assertIn("namespace rggen_demo", generated)
            self.assertIn('.path = "rggen_demo.control"', generated)
            self.assertIn('.path = "rggen_demo.lane[0]"', generated)
            self.assertIn('.path = "rggen_demo.lane[1]"', generated)
            self.assertIn('.path = "rggen_demo.bank.config"', generated)
            self.assertIn('.address = UINT64_C(0x44)', generated)
            self.assertIn("RegisterWriteEffect::WriteOneClear", generated)
            self.assertIn("RegisterReadEffect::Clear", generated)
            self.assertIn('.reset_value = UINT64_C(0xff)', generated)
            self.assertIn('.hdl_path = "u_regs.buffer"', generated)
            self.assertIn('.entries = UINT64_C(4)', generated)
            self.assertIn("RegisterMemoryHandle<Master> buffer", generated)
            self.assertIn('cpptb_signal<"u_regs.control_q">()', generated)

    def test_generated_rggen_model_executes_frontdoors(self) -> None:
        if shutil.which("c++") is None or shutil.which("verilator") is None:
            self.skipTest("C++ and Verilator headers are required")
        verilator = subprocess.run(
            ["verilator", "--getenv", "VERILATOR_ROOT"],
            cwd=REPO,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if verilator.returncode != 0:
            self.skipTest("Verilator include root is unavailable")
        verilator_include = Path(verilator.stdout.strip()) / "include"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / "rggen_registers.hpp"
            export_rggen_model(FIXTURE, output)
            runtime = root / "runtime.cpp"
            runtime.write_text(
                textwrap.dedent(
                    """
                    #include <array>
                    #include "rggen_registers.hpp"

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
                            words.at(request.address / 4) = request.data;
                            co_return write_response_type{};
                        }
                        cpptb::coro::Task<read_response_type> read(
                            read_request_type request) {
                            co_return read_response_type{
                                .data = words.at(request.address / 4)};
                        }
                        std::array<uint32_t, 64> words{};
                    };

                    cpptb::coro::Task<void> exercise(
                        rggen_demo::RegModel<Master>& model, bool& passed) {
                        const auto register_write =
                            co_await model.control.write(0xdu);
                        const auto register_read = co_await model.control.read();
                        const auto memory_write =
                            co_await model.buffer.write(2, 0x12345678u);
                        const auto memory_read = co_await model.buffer.read(2);
                        passed = register_write.okay() && register_read.okay() &&
                                 register_read.data == 0xdu &&
                                 memory_write.okay() && memory_read.okay() &&
                                 memory_read.data == 0x12345678u &&
                                 model.lane.template at<1>().address() == 0x14u &&
                                 model.bank.config.address() == 0x44u;
                    }

                    int main() {
                        cpptb::coro::Testbench scheduler;
                        cpptb::TestResult result;
                        cpptb::TestContext test{scheduler, result};
                        Master master;
                        rggen_demo::RegModel model{test, master};
                        bool passed = false;
                        scheduler.spawn_detached(exercise(model, passed));
                        return scheduler.done() && passed ? 0 : 1;
                    }
                    """
                ),
                encoding="utf-8",
            )
            executable = root / "runtime"
            compiled = subprocess.run(
                [
                    "c++",
                    "-std=c++20",
                    f"-I{REPO / 'include'}",
                    f"-I{verilator_include}",
                    f"-I{verilator_include / 'vltstd'}",
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

    def test_sequence_style_metadata_is_accepted(self) -> None:
        block = extract_rggen_block(
            [
                {
                    "register_block": [
                        {"name": "sequence_demo", "bus_width": 32},
                        {
                            "register": [
                                {"name": "control", "offset_address": 0},
                                {
                                    "bit_fields": [
                                        {
                                            "name": "enable",
                                            "bit_assignment": {"lsb": 0},
                                            "type": "rw",
                                        }
                                    ]
                                },
                            ]
                        },
                    ]
                }
            ]
        )
        self.assertEqual(block.name, "sequence_demo")
        self.assertEqual(block.registers[0].path, "sequence_demo.control")

    def test_json_and_toml_inputs_generate_the_same_model_shape(self) -> None:
        document = {
            "register_blocks": [
                {
                    "name": "portable_demo",
                    "bus_width": 32,
                    "registers": [
                        {
                            "name": "control",
                            "offset_address": 16,
                            "bit_fields": [
                                {
                                    "name": "enable",
                                    "bit_assignment": {"lsb": 0, "width": 1},
                                    "type": "rw",
                                    "initial_value": 1,
                                }
                            ],
                        }
                    ],
                }
            ]
        }
        toml = textwrap.dedent(
            """
            [[register_blocks]]
            name = "portable_demo"
            bus_width = 32

            [[register_blocks.registers]]
            name = "control"
            offset_address = 16

            [[register_blocks.registers.bit_fields]]
            name = "enable"
            type = "rw"
            initial_value = 1
            bit_assignment = { lsb = 0, width = 1 }
            """
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            inputs = {
                "json": json.dumps(document),
                "toml": toml,
            }
            outputs: list[str] = []
            for suffix, contents in inputs.items():
                source = root / f"registers.{suffix}"
                output = root / f"registers_{suffix}.hpp"
                source.write_text(contents, encoding="utf-8")
                export_rggen_model(source, output)
                outputs.append(output.read_text(encoding="utf-8"))

            for generated in outputs:
                self.assertIn("namespace portable_demo", generated)
                self.assertIn('.path = "portable_demo.control"', generated)
                self.assertIn('.address = UINT64_C(0x10)', generated)
                self.assertIn('.reset_value = UINT64_C(0x1)', generated)

    def test_arrayed_register_file_derives_stride_from_its_contents(self) -> None:
        block = extract_rggen_block(
            {
                "register_blocks": [
                    {
                        "name": "arrayed",
                        "bus_width": 32,
                        "register_files": [
                            {
                                "name": "bank",
                                "size": 2,
                                "registers": [
                                    {"name": "control", "offset_address": 0},
                                    {"name": "status", "offset_address": 4},
                                ],
                            }
                        ],
                    }
                ]
            }
        )
        self.assertEqual(
            [register.address for register in block.registers],
            [0x00, 0x04, 0x08, 0x0C],
        )
        self.assertEqual(
            [register.path for register in block.registers],
            [
                "arrayed.bank[0].control",
                "arrayed.bank[0].status",
                "arrayed.bank[1].control",
                "arrayed.bank[1].status",
            ],
        )

    def test_standard_size_step_controls_register_file_stride(self) -> None:
        block = extract_rggen_block(
            {
                "register_blocks": [
                    {
                        "name": "stepped",
                        "bus_width": 32,
                        "register_files": [
                            {
                                "name": "bank",
                                "size": [2, {"step": 0x20}],
                                "registers": [{"name": "control"}],
                            }
                        ],
                    }
                ]
            }
        )
        self.assertEqual(
            [register.address for register in block.registers], [0x00, 0x20]
        )

    def test_hardware_updated_fields_are_volatile(self) -> None:
        block = extract_rggen_block(
            {
                "register_blocks": [
                    {
                        "name": "volatile_fields",
                        "registers": [
                            {
                                "name": "status",
                                "bit_fields": [
                                    {
                                        "name": "changed",
                                        "type": ["rwc", {"reference": "event"}],
                                        "bit_assignment": {"lsb": 0},
                                    },
                                    {
                                        "name": "set",
                                        "type": "rws",
                                        "bit_assignment": {"lsb": 1},
                                    },
                                    {
                                        "name": "enabled",
                                        "type": "rwe",
                                        "bit_assignment": {"lsb": 2},
                                    },
                                ],
                            }
                        ],
                    }
                ]
            }
        )
        self.assertTrue(all(field.volatile for field in block.registers[0].fields))

    def test_overlapping_and_negative_bit_assignments_are_rejected(self) -> None:
        def document(fields):
            return {
                "register_blocks": [
                    {
                        "name": "bad_fields",
                        "registers": [
                            {"name": "control", "bit_fields": fields}
                        ],
                    }
                ]
            }

        with self.assertRaisesRegex(
            RgGenCodegenError, r"bad_fields\.control\.high: .*overlaps"
        ):
            extract_rggen_block(
                document(
                    [
                        {
                            "name": "low",
                            "bit_assignment": {"lsb": 0, "width": 8},
                        },
                        {
                            "name": "high",
                            "bit_assignment": {"lsb": 4, "width": 8},
                        },
                    ]
                )
            )

        with self.assertRaisesRegex(
            RgGenCodegenError, r"bad_fields\.control\.negative: .*exceeds"
        ):
            extract_rggen_block(
                document(
                    [
                        {
                            "name": "negative",
                            "bit_assignment": {"lsb": -1, "width": 1},
                        }
                    ]
                )
            )

    def test_plugin_specific_type_has_path_qualified_error(self) -> None:
        document = {
            "register_blocks": [
                {
                    "name": "bad_block",
                    "registers": [
                        {
                            "name": "control",
                            "bit_fields": [
                                {
                                    "name": "count",
                                    "bit_assignment": {"lsb": 0, "width": 8},
                                    "type": "counter",
                                }
                            ],
                        }
                    ],
                }
            ]
        }
        with self.assertRaisesRegex(
            RgGenCodegenError,
            r"bad_block\.control\.count: unsupported RgGen bit field type",
        ):
            extract_rggen_block(document)

    def test_malformed_yaml_has_a_tool_diagnostic(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "malformed.yml"
            source.write_text("register_blocks: [unterminated\n", encoding="utf-8")
            with self.assertRaisesRegex(
                RgGenCodegenError, r"malformed\.yml: cannot load RgGen metadata"
            ):
                export_rggen_model(source, Path(directory) / "generated.hpp")

    def test_cli_help_describes_native_formats(self) -> None:
        result = subprocess.run(
            ["cpptb-rggen", "--help"],
            cwd=REPO,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("RgGen YAML, JSON, or TOML", result.stdout)
        self.assertIn("--block", result.stdout)


if __name__ == "__main__":
    unittest.main()
