#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class TestEntry:
    suite: str
    target: str
    executable: str
    source: str
    source_size: int
    labels: tuple[str, ...]


def load_manifest(path: Path) -> list[TestEntry]:
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle, delimiter="\t")
        rows: list[TestEntry] = []
        for row in reader:
            labels = tuple(
                label for label in (row.get("labels") or "").split(",") if label
            )
            rows.append(
                TestEntry(
                    suite=row["suite"],
                    target=row["target"],
                    executable=row["executable"],
                    source=row["source"],
                    source_size=max(1, int(row["source_size"])),
                    labels=labels,
                )
            )
    return rows


def filter_entries(entries: list[TestEntry], suites: set[str]) -> list[TestEntry]:
    if not suites:
        return list(entries)
    return [entry for entry in entries if entry.suite in suites]


def shard_entries(
    entries: list[TestEntry], shard_count: int, shard_index: int
) -> list[TestEntry]:
    shards: list[list[TestEntry]] = [[] for _ in range(shard_count)]
    shard_weights = [0 for _ in range(shard_count)]

    ordered = sorted(
        entries,
        key=lambda entry: (-entry.source_size, entry.target, entry.source),
    )

    for entry in ordered:
        lightest_index = min(
            range(shard_count),
            key=lambda index: (shard_weights[index], len(shards[index]), index),
        )
        shards[lightest_index].append(entry)
        shard_weights[lightest_index] += entry.source_size

    return sorted(shards[shard_index], key=lambda entry: (entry.target, entry.source))


def emit_entries(entries: list[TestEntry], field: str) -> int:
    for entry in entries:
        print(getattr(entry, field))
    return 0


def run_entries(entries: list[TestEntry], reporter: str, timeout_seconds: int) -> int:
    for entry in entries:
        executable = Path(entry.executable)
        if not executable.exists():
            print(
                f"[test-shard] FAIL missing executable for target {entry.target}: "
                f"{entry.executable}",
                file=sys.stderr,
                flush=True,
            )
            return 1

        print(
            f"[test-shard] RUN {entry.target} "
            f"({entry.suite}, weight={entry.source_size})",
            flush=True,
        )
        try:
            completed = subprocess.run(
                [str(executable), "--reporter", reporter],
                check=False,
                timeout=timeout_seconds,
            )
        except subprocess.TimeoutExpired:
            print(
                f"[test-shard] FAIL target {entry.target} timed out after "
                f"{timeout_seconds}s",
                file=sys.stderr,
                flush=True,
            )
            return 124
        if completed.returncode != 0:
            print(
                f"[test-shard] FAIL target {entry.target} exited with "
                f"{completed.returncode}",
                file=sys.stderr,
                flush=True,
            )
            return completed.returncode

    print(f"[test-shard] PASS {len(entries)} test targets", flush=True)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Select or run one deterministic shard from a test manifest."
    )
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--shard-count", type=int, required=True)
    parser.add_argument("--shard-index", type=int, required=True)
    parser.add_argument("--suite", action="append", default=[])
    parser.add_argument(
        "--emit",
        choices=("targets", "executables", "sources"),
        default="targets",
    )
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--reporter", default="compact")
    parser.add_argument("--timeout-seconds", type=int, default=120)
    args = parser.parse_args()

    if args.shard_count <= 0:
        print("[test-shard] FAIL shard-count must be positive", file=sys.stderr)
        return 2
    if args.shard_index < 0 or args.shard_index >= args.shard_count:
        print("[test-shard] FAIL shard-index out of range", file=sys.stderr)
        return 2

    manifest_path = args.manifest.resolve()
    if not manifest_path.exists():
        print(f"[test-shard] FAIL missing manifest: {manifest_path}", file=sys.stderr)
        return 2

    entries = filter_entries(load_manifest(manifest_path), set(args.suite))
    if not entries:
        print("[test-shard] FAIL manifest filter selected no tests", file=sys.stderr)
        return 2

    shard = shard_entries(entries, args.shard_count, args.shard_index)
    if not shard:
        print("[test-shard] FAIL computed shard is empty", file=sys.stderr)
        return 2

    if args.run:
      return run_entries(shard, args.reporter, args.timeout_seconds)

    field_name = {
        "targets": "target",
        "executables": "executable",
        "sources": "source",
    }[args.emit]
    return emit_entries(shard, field_name)


if __name__ == "__main__":
    sys.exit(main())
