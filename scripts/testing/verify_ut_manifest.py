#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def header_to_ut(include_root: Path, ut_root: Path, header: Path) -> Path:
    rel = header.relative_to(include_root)
    return ut_root / include_root.name / rel.parent / f"{rel.stem}_ut.cpp"


def ut_to_header(include_root: Path, ut_root: Path, ut_file: Path) -> Path:
    rel = ut_file.relative_to(ut_root)
    if not rel.parts or rel.parts[0] != include_root.name:
        return include_root / "__unexpected__"
    rel = Path(*rel.parts[1:])
    if rel.name.endswith("_ut.cpp"):
        header_name = f"{rel.stem[:-3]}.hpp"
    else:
        header_name = f"{rel.stem}.hpp"
    return include_root / rel.parent / header_name


def collect_headers(include_root: Path) -> list[Path]:
    return sorted(path for path in include_root.rglob("*.hpp") if path.is_file())


def collect_ut_files(ut_root: Path) -> list[Path]:
    return sorted(path for path in ut_root.rglob("*_ut.cpp") if path.is_file())


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify UT/header path mapping.")
    parser.add_argument("--include-root", type=Path, required=True)
    parser.add_argument("--ut-root", type=Path, required=True)
    parser.add_argument("--verify-existing", action="store_true")
    parser.add_argument("--report-missing", action="store_true")
    parser.add_argument("--strict", action="store_true")
    parser.add_argument("--summary-json", type=Path)
    args = parser.parse_args()

    include_root = args.include_root.resolve()
    ut_root = args.ut_root.resolve()

    headers = collect_headers(include_root)
    ut_files = collect_ut_files(ut_root)

    unexpected_ut: list[Path] = []
    missing_ut: list[Path] = []

    if args.verify_existing:
        for ut_file in ut_files:
            header = ut_to_header(include_root, ut_root, ut_file)
            if not header.exists():
                unexpected_ut.append(ut_file)

    if args.report_missing or args.strict:
        for header in headers:
            ut_path = header_to_ut(include_root, ut_root, header)
            if not ut_path.exists():
                missing_ut.append(header)

    print(f"headers: {len(headers)}")
    print(f"ut_files: {len(ut_files)}")

    if unexpected_ut:
        print("unexpected_ut_files:")
        for path in unexpected_ut:
            print(f"  {path.relative_to(ut_root)}")

    if missing_ut and (args.report_missing or args.strict):
        print("missing_ut_files:")
        for path in missing_ut:
            print(f"  {path.relative_to(include_root)}")

    if args.summary_json is not None:
        summary_path = args.summary_json.resolve()
        summary_path.parent.mkdir(parents=True, exist_ok=True)
        summary = {
            "include_root": str(include_root),
            "ut_root": str(ut_root),
            "headers": len(headers),
            "ut_files": len(ut_files),
            "unexpected_ut_files": [
                str(path.relative_to(ut_root)) for path in unexpected_ut
            ],
            "missing_ut_files": [
                str(path.relative_to(include_root)) for path in missing_ut
            ],
        }
        summary_path.write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

    if unexpected_ut:
        return 1
    if args.strict and missing_ut:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
