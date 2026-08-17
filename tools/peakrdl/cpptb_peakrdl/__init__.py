"""cpptb's SystemRDL/IP-XACT register-model exporter.

Two deliberately separate modules: `codegen` is the stdlib-only emitter that
the main toolchain also uses for `cpptb-rggen`, and `plugin` is the thin
adapter that registers it with a user-installed PeakRDL. The split keeps the
LGPL/GPL PeakRDL packages out of every required dependency graph -- they are
extras here and never bundled into cpptb binaries.
"""

__version__ = "0.1.0"
