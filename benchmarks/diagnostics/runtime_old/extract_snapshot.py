#!/usr/bin/env python3
"""Reconstruct the pre-feature runtime snapshot from Claude transcript JSONL."""

import argparse
import hashlib
import json
from pathlib import Path


TRANSCRIPT_DIR = Path(
    "/Users/amann/.claude/projects/-Users-amann-Documents-mojo-hdl-sim"
)
SNAPSHOT_ROOT = Path(__file__).resolve().parent

CAPTURES = (
    {
        "snapshot_path": "cpptb/coro_runtime.hpp",
        "transcript": "0c58d587-ebb9-41e6-9105-cba62154b306.jsonl",
        "result_uuid": "29c37513-5e0f-41f9-9743-6ca781d94aba",
        "source_path": "/Users/amann/Documents/mojo-hdl-sim/cpptb/coro_runtime.hpp",
    },
    {
        "snapshot_path": (
            "benchmarks/peripheral_suite/cpp_dpi/framework/"
            "peripheral_suite_fixture.cpp"
        ),
        "transcript": "23f51669-8a36-456c-b144-bee86bc3a8d7.jsonl",
        "result_uuid": "a25f3149-8816-419e-9bc8-62f1acb2d21f",
        "source_path": (
            "/Users/amann/Documents/mojo-hdl-sim/benchmarks/peripheral_suite/"
            "cpp_dpi/framework/peripheral_suite_fixture.cpp"
        ),
    },
    {
        "snapshot_path": (
            "benchmarks/peripheral_suite/cpp_dpi/framework/"
            "peripheral_suite_fixture.hpp"
        ),
        "transcript": "0c58d587-ebb9-41e6-9105-cba62154b306.jsonl",
        "result_uuid": "7ebb36ce-dc8b-453e-b93a-e40c77de8afb",
        "source_path": (
            "/Users/amann/Documents/mojo-hdl-sim/benchmarks/peripheral_suite/"
            "cpp_dpi/framework/peripheral_suite_fixture.hpp"
        ),
    },
    {
        "snapshot_path": (
            "benchmarks/peripheral_suite/cpp_dpi/framework/peripheral_suite.hpp"
        ),
        "transcript": "5b8c1b42-3e95-44b2-a8fe-08a247ba9880.jsonl",
        "result_uuid": "8c145fd9-0a71-455f-ac6a-c81e8b268810",
        "source_path": (
            "/Users/amann/Documents/mojo-hdl-sim/benchmarks/peripheral_suite/"
            "cpp_dpi/framework/peripheral_suite.hpp"
        ),
    },
    {
        "snapshot_path": "benchmarks/peripheral_suite/cpp_dpi/testbench.cpp",
        "transcript": "0c58d587-ebb9-41e6-9105-cba62154b306.jsonl",
        "result_uuid": "9ffa1192-5208-45d8-b95e-0f8d335a6a4f",
        "source_path": (
            "/Users/amann/Documents/mojo-hdl-sim/benchmarks/peripheral_suite/"
            "cpp_dpi/testbench.cpp"
        ),
    },
)

RUNTIME_CONFIRMATIONS = (
    {
        "transcript": "23f51669-8a36-456c-b144-bee86bc3a8d7.jsonl",
        "result_uuid": "9987fd4d-487b-455a-aac6-9178f69bb864",
    },
    {
        "transcript": "242a276e-ff9b-4fdf-9287-c2d8624170ea.jsonl",
        "result_uuid": "6a330658-c8d4-4786-90eb-faa56868c218",
    },
)

EXCLUDED_RUNTIME = {
    "transcript": "ea7ebcea-ac15-4e3a-b2ab-b6a52c608341.jsonl",
    "result_uuid": "ac4f30e6-505d-43d2-8d25-51208e306a83",
}


def _records(path):
    with path.open(encoding="utf-8") as stream:
        return [json.loads(line) for line in stream]


def extract_capture(transcript_dir, descriptor):
    transcript_path = transcript_dir / descriptor["transcript"]
    records = _records(transcript_path)
    record = next(
        (item for item in records if item.get("uuid") == descriptor["result_uuid"]),
        None,
    )
    if record is None:
        raise RuntimeError(
            f"missing result UUID {descriptor['result_uuid']} in {transcript_path}"
        )

    file_result = record.get("toolUseResult", {}).get("file", {})
    content = file_result.get("content")
    if not isinstance(content, str):
        raise RuntimeError(f"capture has no toolUseResult.file.content: {descriptor}")
    expected_path = descriptor.get("source_path")
    if expected_path and file_result.get("filePath") != expected_path:
        raise RuntimeError(
            f"capture path mismatch: {file_result.get('filePath')} != {expected_path}"
        )

    assistant_uuid = record.get("sourceToolAssistantUUID")
    assistant = next(
        (item for item in records if item.get("uuid") == assistant_uuid), None
    )
    tool_calls = [] if assistant is None else assistant.get("message", {}).get(
        "content", []
    )
    read_call = next(
        (
            block
            for block in tool_calls
            if isinstance(block, dict)
            and block.get("type") == "tool_use"
            and block.get("name") == "Read"
            and block.get("input", {}).get("file_path") == file_result.get("filePath")
        ),
        None,
    )

    encoded = content.encode("utf-8")
    metadata = {
        "transcript_path": str(transcript_path),
        "transcript_line": records.index(record) + 1,
        "session_id": record.get("sessionId"),
        "result_uuid": record.get("uuid"),
        "source_tool_assistant_uuid": assistant_uuid,
        "tool_use_id": None if read_call is None else read_call.get("id"),
        "source_file_path": file_result.get("filePath"),
        "reported_line_count": file_result.get("numLines"),
        "line_count": content.count("\n") + (1 if content else 0),
        "byte_count": len(encoded),
        "sha256": hashlib.sha256(encoded).hexdigest(),
    }
    return encoded, metadata


def reconstruct(transcript_dir=TRANSCRIPT_DIR, snapshot_root=SNAPSHOT_ROOT, check=False):
    files = []
    source_bytes = {}
    for descriptor in CAPTURES:
        content, metadata = extract_capture(transcript_dir, descriptor)
        relative_path = descriptor["snapshot_path"]
        metadata["snapshot_path"] = relative_path
        files.append(metadata)
        source_bytes[relative_path] = content

    runtime_path = "cpptb/coro_runtime.hpp"
    confirmation_records = []
    for descriptor in RUNTIME_CONFIRMATIONS:
        content, metadata = extract_capture(
            transcript_dir,
            {**descriptor, "source_path": CAPTURES[0]["source_path"]},
        )
        metadata["byte_identical_to_accepted"] = content == source_bytes[runtime_path]
        if not metadata["byte_identical_to_accepted"]:
            raise RuntimeError("the accepted runtime captures are not byte-identical")
        confirmation_records.append(metadata)

    excluded_content, excluded_metadata = extract_capture(
        transcript_dir,
        {**EXCLUDED_RUNTIME, "source_path": CAPTURES[0]["source_path"]},
    )
    excluded_metadata["reason"] = (
        "Older 1,071-line capture; not used as the primary reconstruction."
    )
    excluded_metadata["byte_identical_to_accepted"] = (
        excluded_content == source_bytes[runtime_path]
    )

    provenance = {
        "schema_version": 1,
        "extraction_method": (
            "Exact UTF-8 bytes from each JSONL record's "
            "toolUseResult.file.content field; no semantic edits."
        ),
        "snapshot_root": str(snapshot_root),
        "files": files,
        "runtime_confirmation_captures": confirmation_records,
        "excluded_runtime_capture": excluded_metadata,
        "ambiguity": {
            "accepted_skip_build_binary_exact_source": "ambiguous",
            "historical_binary": "unrecoverable",
            "note": (
                "The reconstructed 1,230-line source is independently corroborated, "
                "but no recoverable historical binary proves that the accepted "
                "--skip-build run used these exact bytes."
            ),
        },
    }
    provenance_bytes = (json.dumps(provenance, indent=2) + "\n").encode("utf-8")

    mismatches = []
    for relative_path, content in source_bytes.items():
        output_path = snapshot_root / relative_path
        if check:
            if not output_path.exists() or output_path.read_bytes() != content:
                mismatches.append(relative_path)
        else:
            output_path.parent.mkdir(parents=True, exist_ok=True)
            output_path.write_bytes(content)

    provenance_path = snapshot_root / "provenance.json"
    if check:
        if not provenance_path.exists() or provenance_path.read_bytes() != provenance_bytes:
            mismatches.append("provenance.json")
        if mismatches:
            raise RuntimeError("snapshot mismatch: " + ", ".join(mismatches))
    else:
        provenance_path.write_bytes(provenance_bytes)

    return provenance


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--transcript-dir", type=Path, default=TRANSCRIPT_DIR)
    parser.add_argument("--snapshot-root", type=Path, default=SNAPSHOT_ROOT)
    args = parser.parse_args(argv)
    provenance = reconstruct(args.transcript_dir, args.snapshot_root, args.check)
    action = "Verified" if args.check else "Reconstructed"
    print(f"{action} {len(provenance['files'])} files in {args.snapshot_root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
