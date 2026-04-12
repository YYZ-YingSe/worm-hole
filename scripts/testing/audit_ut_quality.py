#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


UT_TAG_RE = re.compile(r"\[UT\]\[([^\]]+)\]\[([^\]]+)\]")

CONCURRENCY_CANDIDATE_PATTERNS = (
    "wh/core/bounded_queue/**/*.hpp",
    "wh/core/cursor_reader/**/*.hpp",
    "wh/core/stdexec/**/*.hpp",
    "wh/schema/stream/core/**/*.hpp",
    "wh/schema/stream/reader/**/*.hpp",
    "wh/schema/stream/writer/**/*.hpp",
    "wh/sync/async_mutex.hpp",
    "wh/compose/graph/detail/**/*.hpp",
    "wh/compose/node/detail/tools/**/*.hpp",
)

CONCURRENCY_SIGNAL_KEYWORDS = (
    "std::atomic",
    "compare_exchange",
    "request_drive",
    "request_stop",
    "stop_token",
    "condition_variable",
    "notify_",
)


def header_to_ut(include_root: Path, ut_root: Path, header: Path) -> Path:
    rel = header.relative_to(include_root)
    return ut_root / include_root.name / rel.parent / f"{rel.stem}_ut.cpp"


def collect_headers(include_root: Path) -> list[Path]:
    return sorted(path for path in include_root.rglob("*.hpp") if path.is_file())


def collect_ut_files(ut_root: Path) -> list[Path]:
    return sorted(path for path in ut_root.rglob("*_ut.cpp") if path.is_file())


def header_relpath(include_root: Path, header: Path) -> str:
    return f"{include_root.name}/{header.relative_to(include_root).as_posix()}"


def is_concurrency_candidate(rel_header: str, text: str) -> bool:
    header_path = Path(rel_header)
    if not any(header_path.match(pattern) for pattern in CONCURRENCY_CANDIDATE_PATTERNS):
        return False
    return any(keyword in text for keyword in CONCURRENCY_SIGNAL_KEYWORDS)


def analyze_ut_file(include_root: Path, header: Path, ut_file: Path) -> dict[str, object]:
    text = ut_file.read_text(encoding="utf-8")
    rel_header = header_relpath(include_root, header)
    tags = UT_TAG_RE.findall(text)
    tagged_paths = sorted({path for path, _ in tags})
    tagged_symbols = sorted({symbol for _, symbol in tags})

    return {
        "header": rel_header,
        "ut_file": ut_file.as_posix(),
        "has_ut_tag": bool(tags),
        "path_match": rel_header in tagged_paths,
        "tagged_paths": tagged_paths,
        "tagged_symbols": tagged_symbols,
        "has_condition": "[condition]" in text,
        "has_branch": "[branch]" in text,
        "has_boundary": "[boundary]" in text,
        "has_concurrency": "[concurrency]" in text,
        "concurrency_candidate": is_concurrency_candidate(rel_header, text),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Audit UT quality and tag coverage.")
    parser.add_argument("--include-root", type=Path, required=True)
    parser.add_argument("--ut-root", type=Path, required=True)
    parser.add_argument("--strict", action="store_true")
    parser.add_argument("--strict-concurrency", action="store_true")
    parser.add_argument("--summary-json", type=Path)
    args = parser.parse_args()

    include_root = args.include_root.resolve()
    ut_root = args.ut_root.resolve()

    headers = collect_headers(include_root)
    ut_files = collect_ut_files(ut_root)

    missing_ut: list[str] = []
    missing_ut_tag: list[str] = []
    path_mismatch: list[str] = []
    missing_condition: list[str] = []
    missing_branch: list[str] = []
    missing_boundary: list[str] = []
    concurrency_candidates_without_tag: list[str] = []

    analyzed_files: list[dict[str, object]] = []

    for header in headers:
        ut_file = header_to_ut(include_root, ut_root, header)
        rel_header = header_relpath(include_root, header)
        if not ut_file.exists():
            missing_ut.append(rel_header)
            continue

        analyzed = analyze_ut_file(include_root, header, ut_file)
        analyzed_files.append(analyzed)

        if not analyzed["has_ut_tag"]:
            missing_ut_tag.append(rel_header)
        if not analyzed["path_match"]:
            path_mismatch.append(rel_header)
        if not analyzed["has_condition"]:
            missing_condition.append(rel_header)
        if not analyzed["has_branch"]:
            missing_branch.append(rel_header)
        if not analyzed["has_boundary"]:
            missing_boundary.append(rel_header)
        if analyzed["concurrency_candidate"] and not analyzed["has_concurrency"]:
            concurrency_candidates_without_tag.append(rel_header)

    print(f"headers: {len(headers)}")
    print(f"ut_files: {len(ut_files)}")
    print(f"missing_ut: {len(missing_ut)}")
    print(f"missing_ut_tag: {len(missing_ut_tag)}")
    print(f"path_mismatch: {len(path_mismatch)}")
    print(f"missing_condition: {len(missing_condition)}")
    print(f"missing_branch: {len(missing_branch)}")
    print(f"missing_boundary: {len(missing_boundary)}")
    print(
        "concurrency_candidates_without_tag: "
        f"{len(concurrency_candidates_without_tag)}"
    )

    def print_group(title: str, items: list[str]) -> None:
        if not items:
            return
        print(f"{title}:")
        for item in items:
            print(f"  {item}")

    print_group("missing_ut_files", missing_ut)
    print_group("missing_ut_tags", missing_ut_tag)
    print_group("path_mismatch_files", path_mismatch)
    print_group("missing_condition_files", missing_condition)
    print_group("missing_branch_files", missing_branch)
    print_group("missing_boundary_files", missing_boundary)
    print_group(
        "concurrency_candidates_without_tag_files",
        concurrency_candidates_without_tag,
    )

    if args.summary_json is not None:
        summary_path = args.summary_json.resolve()
        summary_path.parent.mkdir(parents=True, exist_ok=True)
        summary = {
            "include_root": str(include_root),
            "ut_root": str(ut_root),
            "headers": len(headers),
            "ut_files": len(ut_files),
            "missing_ut_files": missing_ut,
            "missing_ut_tags": missing_ut_tag,
            "path_mismatch_files": path_mismatch,
            "missing_condition_files": missing_condition,
            "missing_branch_files": missing_branch,
            "missing_boundary_files": missing_boundary,
            "concurrency_candidates_without_tag_files": (
                concurrency_candidates_without_tag
            ),
            "files": analyzed_files,
        }
        summary_path.write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

    if not args.strict and not args.strict_concurrency:
        return 0

    failed = False
    if missing_ut or missing_ut_tag or path_mismatch:
        failed = True
    if args.strict and (missing_condition or missing_branch or missing_boundary):
        failed = True
    if args.strict_concurrency and concurrency_candidates_without_tag:
        failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
