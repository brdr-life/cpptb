"""Elaboration frontend registry for DPI binding generation."""

from __future__ import annotations

from pathlib import Path
from typing import Any, Protocol

from cpptb.codegen.design_ir import CodegenError, DesignIR


class DesignFrontend(Protocol):
    name: str

    def elaborate(self, manifest: dict[str, Any], base_dir: Path) -> DesignIR: ...


def frontend_name(manifest: dict[str, Any], override: str | None = None) -> str:
    if override is not None:
        return override
    config = manifest.get("frontend", {})
    if isinstance(config, str):
        return config
    if not isinstance(config, dict):
        raise CodegenError("manifest frontend must be a string or object")
    return config.get("kind", "slang")


def frontend_options(manifest: dict[str, Any], name: str) -> dict[str, Any]:
    all_options = manifest.get("frontend_options", {})
    if not isinstance(all_options, dict):
        raise CodegenError("manifest frontend_options must be an object")
    options = all_options.get(name, {})
    if not isinstance(options, dict):
        raise CodegenError(f"manifest frontend_options.{name} must be an object")

    # Accept the original inline object form while manifests migrate to the
    # backend-keyed frontend_options table.
    selected = manifest.get("frontend", {})
    if isinstance(selected, dict) and selected.get("kind", "slang") == name:
        inline = {key: value for key, value in selected.items() if key != "kind"}
        return {**options, **inline}
    return options


def get_frontend(name: str) -> DesignFrontend:
    if name == "slang":
        from cpptb.codegen.frontends.slang import SlangFrontend

        return SlangFrontend()
    if name == "verilator_json":
        from cpptb.codegen.frontends.verilator_json import VerilatorJsonFrontend

        return VerilatorJsonFrontend()
    raise CodegenError(
        f"unknown elaboration frontend {name!r}; expected 'slang' or 'verilator_json'"
    )


def elaborate_design(
    manifest: dict[str, Any], base_dir: Path, override: str | None = None
) -> DesignIR:
    return get_frontend(frontend_name(manifest, override)).elaborate(manifest, base_dir)
