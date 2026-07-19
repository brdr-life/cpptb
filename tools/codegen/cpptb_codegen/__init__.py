"""SystemVerilog elaboration and DPI binding generation for cpptb."""

__version__ = "0.1.0"

from .design_ir import CodegenError, DesignIR
from .generate_dpi_bindings import generate

__all__ = ["CodegenError", "DesignIR", "generate", "__version__"]
