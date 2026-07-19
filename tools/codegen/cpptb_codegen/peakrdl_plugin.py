"""PeakRDL exporter plugin for cpptb C++ register models."""

from __future__ import annotations

import argparse
from typing import TYPE_CHECKING

from peakrdl.plugins.exporter import ExporterSubcommandPlugin

from cpptb_codegen.register_codegen import export_register_model

if TYPE_CHECKING:
    from systemrdl.node import AddrmapNode


class CppTbExporter(ExporterSubcommandPlugin):
    short_desc = "Generate a cpptb C++ register model"

    def add_exporter_arguments(self, arg_group: argparse._ActionsContainer) -> None:
        arg_group.add_argument(
            "--namespace",
            help=(
                "C++ namespace only (default: sanitized top-level instance "
                "name)"
            ),
        )
        arg_group.add_argument(
            "--class-name",
            default="RegModel",
            help="generated C++ register-model class name [RegModel]",
        )
        arg_group.add_argument(
            "--register-endianness",
            choices=("little", "big"),
            default="little",
            help="byte order for split register frontdoor transfers [little]",
        )

    def do_export(self, top_node: AddrmapNode, options: argparse.Namespace) -> None:
        export_register_model(
            top_node,
            options.output,
            namespace=options.namespace,
            class_name=options.class_name,
            endianness=options.register_endianness,
        )
