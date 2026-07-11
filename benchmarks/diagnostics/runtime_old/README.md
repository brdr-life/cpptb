# Reconstructed pre-feature runtime snapshot

This tree mirrors repository include paths for an isolated diagnostic build of
the peripheral-suite C++ DPI executable. The source files are mechanically
extracted from Claude transcript JSONL `toolUseResult.file.content` fields by
`extract_snapshot.py`; they are not hand-edited adaptations of the current
sources.

The accepted coroutine runtime is the 1,230-line capture. Independent captures
in sessions `0c58d587`, `23f51669`, and `242a276e` are byte-identical. The older
1,071-line `ea7ebcea` capture is recorded but deliberately excluded. Structured
captures for the fixture pieces are split across sessions, which is represented
file-by-file in `provenance.json`.

The historical executable used by the accepted `--skip-build` result cannot be
recovered. Consequently, its exact source remains ambiguous: this snapshot is a
corroborated reconstruction, not proof that the lost binary was built from these
exact bytes.

Recreate or verify the snapshot with:

```sh
python3 benchmarks/diagnostics/runtime_old/extract_snapshot.py
python3 benchmarks/diagnostics/runtime_old/extract_snapshot.py --check
```

Build only the isolated diagnostic executable with:

```sh
make peripheral-suite-runtime-old-diagnostic-build
python3 benchmarks/diagnostics/runtime_old/record_build.py
```

The build uses current RTL, generated DPI wrapper, `dpi_runtime.hpp`, transport,
and benchmark sequence implementation. Its include search puts this snapshot
root before the repository root, selecting the old runtime and old fixture API.
`build_provenance.json` records the exact Verilator argument vector, compiler
and Verilator versions, input hashes, and resulting binary SHA-256 for the
documented reconstruction build.
