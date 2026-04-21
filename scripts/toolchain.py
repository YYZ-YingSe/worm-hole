#!/usr/bin/env python3

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import json
import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


ROOT = Path(__file__).resolve().parent.parent
THIRD_PARTY_ROOT = ROOT / "thirdy_party"
BUILD_ROOT = ROOT / "build"
TESTS_DIR = ROOT / "tests"
DEFAULT_REPORTER = "compact"
DEFAULT_LOCAL_PRESET = "dev-debug"
DEFAULT_EDITOR_PRESET = "dev-editor"
DEFAULT_ANALYSIS_PRESET = "ci-static-analysis"
DEFAULT_SANITIZER_PRESET = "ci-asan-ubsan"
DEFAULT_COVERAGE_PRESET = "ci-coverage"


@dataclass(frozen=True)
class TestEntry:
    suite: str
    target: str
    executable: Path
    source: Path
    source_size: int
    weight: int
    timeout_seconds: int
    labels: tuple[str, ...]


@dataclass(frozen=True)
class CompileEntry:
    file: Path
    size: int


@dataclass(frozen=True)
class MatrixLane:
    name: str
    configure_preset: str
    build_preset: str
    min_shards: int
    suites: tuple[str, ...]
    include_labels: tuple[str, ...]
    exclude_labels: tuple[str, ...]
    os: str | None = None


@dataclass(frozen=True)
class TestBuildCostModel:
    fixed_compile_actions: int
    fixed_link_actions: int
    per_target_compile_actions: int = 1
    per_target_link_actions: int = 1

    @property
    def fixed_actions(self) -> int:
        return self.fixed_compile_actions + self.fixed_link_actions

    def estimate_actions_for_targets(self, target_count: int) -> int:
        return (
            self.fixed_actions
            + target_count * self.per_target_compile_actions
            + target_count * self.per_target_link_actions
        )


def print_step(tag: str, message: str) -> None:
    print(f"[{tag}] {message}", flush=True)


def fail(tag: str, message: str, code: int = 1) -> None:
    print(f"[{tag}] FAIL {message}", file=sys.stderr, flush=True)
    raise SystemExit(code)


def run(
    cmd: Sequence[str],
    *,
    cwd: Path = ROOT,
    env: dict[str, str] | None = None,
    check: bool = True,
    capture_output: bool = False,
) -> subprocess.CompletedProcess[str]:
    merged_env = os.environ.copy()
    if env:
        merged_env.update(env)
    return subprocess.run(
        list(cmd),
        cwd=str(cwd),
        env=merged_env,
        check=check,
        text=True,
        capture_output=capture_output,
    )


def print_process_output(result: subprocess.CompletedProcess[str]) -> None:
    if result.stdout:
        print(result.stdout, end="" if result.stdout.endswith("\n") else "\n")
    if result.stderr:
        print(
            result.stderr,
            end="" if result.stderr.endswith("\n") else "\n",
            file=sys.stderr,
        )


def command_exists(name: str) -> bool:
    return shutil.which(name) is not None


def require_commands(tag: str, *commands: str) -> None:
    missing = [command for command in commands if not command_exists(command)]
    if missing:
        fail(tag, f"missing required tool: {', '.join(missing)}")


def under(path: Path, base: Path) -> bool:
    try:
        path.relative_to(base)
        return True
    except ValueError:
        return False


def is_project_path_in_scope(
    path: Path,
    *,
    include_tests: bool = True,
    include_third_party: bool = False,
) -> bool:
    if not path.exists():
        return False
    if not under(path, ROOT):
        return False
    if not include_third_party and under(path, THIRD_PARTY_ROOT):
        return False
    if under(path, BUILD_ROOT):
        return False
    if not include_tests and under(path, TESTS_DIR):
        return False
    return True


def resolve_compile_entry_file(entry: dict[str, object]) -> Path | None:
    file_value = entry.get("file")
    if not file_value:
        return None

    file_path = Path(str(file_value))
    if not file_path.is_absolute():
        directory = Path(str(entry.get("directory", ROOT)))
        file_path = (directory / file_path).resolve()
    else:
        file_path = file_path.resolve()
    return file_path


def build_dir_for_preset(preset: str) -> Path:
    return BUILD_ROOT / preset


def sync_compile_commands(build_dir: Path) -> None:
    source = build_dir / "compile_commands.json"
    target = ROOT / "compile_commands.json"
    if not source.exists():
        return
    shutil.copyfile(source, target)
    print_step("compile-commands", f"SYNC {source} -> {target}")


def editor_compile_database_is_stale() -> bool:
    editor_build_dir = build_dir_for_preset(DEFAULT_EDITOR_PRESET)
    compile_db = editor_build_dir / "compile_commands.json"
    if not compile_db.exists():
        return True

    compile_db_mtime = compile_db.stat().st_mtime
    watched_roots = [
        ROOT / "CMakeLists.txt",
        ROOT / "CMakePresets.json",
        ROOT / ".clangd",
        ROOT / "cmake",
        ROOT / "tests",
        ROOT / "example",
        ROOT / "benchmark",
    ]

    for path in watched_roots:
        if not path.exists():
            continue
        if path.is_file():
            if path.stat().st_mtime > compile_db_mtime:
                return True
            continue

        for child in path.rglob("*"):
            if not child.is_file():
                continue
            if child.stat().st_mtime > compile_db_mtime:
                return True
    return False


def ensure_editor_compile_database(tag: str) -> None:
    if os.environ.get("WH_SKIP_EDITOR_SYNC") == "1":
        print_step("editor", "SKIP disabled by WH_SKIP_EDITOR_SYNC=1")
        return

    if not editor_compile_database_is_stale():
        print_step("editor", f"READY {build_dir_for_preset(DEFAULT_EDITOR_PRESET)}")
        return

    print_step("editor", f"CONFIGURE {DEFAULT_EDITOR_PRESET}")
    build_dir = configure_preset(
        DEFAULT_EDITOR_PRESET,
        f"{tag}-editor",
    )
    print_step("editor", f"PASS {build_dir}")


def compiler_cache_bin() -> str | None:
    candidates = [
        os.environ.get("WH_COMPILER_CACHE_BIN"),
        os.environ.get("SCCACHE_PATH"),
        shutil.which("sccache"),
    ]
    for candidate in candidates:
        if candidate and Path(candidate).exists():
            return candidate
    return None


def zero_compiler_cache_stats(tag: str) -> None:
    cache_bin = compiler_cache_bin()
    if not cache_bin:
        return
    print_step(tag, "sccache enabled")
    subprocess.run([cache_bin, "--zero-stats"], check=False, text=True)


def print_compiler_cache_stats() -> None:
    cache_bin = compiler_cache_bin()
    if not cache_bin:
        return
    subprocess.run([cache_bin, "--show-stats"], check=False, text=True)


def configure_preset(
    preset: str,
    tag: str,
    *,
    cache_entries: Sequence[str] = (),
) -> Path:
    require_commands(tag, "cmake")
    args = ["cmake", "--preset", preset, *cache_entries]
    cache_bin = compiler_cache_bin()
    if cache_bin:
        zero_compiler_cache_stats(tag)
        args.append(f"-DCMAKE_CXX_COMPILER_LAUNCHER={cache_bin}")
    run(args)
    build_dir = build_dir_for_preset(preset)
    sync_compile_commands(build_dir)
    return build_dir


def build_preset(
    preset: str,
    tag: str,
    *,
    targets: Sequence[str] = (),
    jobs: str | None = None,
) -> None:
    require_commands(tag, "cmake")
    args = ["cmake", "--build", "--preset", preset]
    if jobs:
        args.extend(["--parallel", jobs])
    else:
        args.append("--parallel")
    if targets:
        args.append("--target")
        args.extend(targets)
    run(args)


def max_build_targets_per_batch() -> int:
    raw = os.environ.get("CI_MAX_BUILD_TARGETS_PER_BATCH")
    if raw is None:
        return 200
    try:
        value = int(raw)
    except ValueError:
        fail("toolchain", f"invalid CI_MAX_BUILD_TARGETS_PER_BATCH: {raw}", code=2)
    if value <= 0:
        fail("toolchain", "CI_MAX_BUILD_TARGETS_PER_BATCH must be positive", code=2)
    return value


def build_targets_in_batches(
    preset: str,
    tag: str,
    *,
    targets: Sequence[str],
    jobs: str | None = None,
    max_targets_per_batch: int | None = None,
) -> None:
    target_list = list(targets)
    if not target_list:
        build_preset(preset, tag, jobs=jobs)
        return

    batch_limit = max_targets_per_batch or max_build_targets_per_batch()
    if len(target_list) <= batch_limit:
        build_preset(preset, tag, targets=target_list, jobs=jobs)
        return

    batch_count = (len(target_list) + batch_limit - 1) // batch_limit
    for batch_index, start in enumerate(range(0, len(target_list), batch_limit), start=1):
        batch_targets = target_list[start : start + batch_limit]
        print_step(
            tag,
            f"build batch {batch_index}/{batch_count} targets={len(batch_targets)}",
        )
        build_preset(preset, tag, targets=batch_targets, jobs=jobs)


def build_artifact_target(kind: str) -> str:
    if kind == "enabled":
        return "wh_enabled_artifacts"
    if kind == "tests":
        return "wh_test_artifacts"
    raise ValueError(f"unsupported artifact kind: {kind}")


def resolve_diff_base() -> str | None:
    candidates: list[str] = []

    diff_base_sha = os.environ.get("WH_CI_DIFF_BASE_SHA")
    if diff_base_sha:
        candidates.append(diff_base_sha)

    event_path = os.environ.get("GITHUB_EVENT_PATH")
    if event_path:
        try:
            payload = json.loads(Path(event_path).read_text())
        except Exception:
            payload = {}

        pull_request = payload.get("pull_request")
        if isinstance(pull_request, dict):
            base = pull_request.get("base")
            if isinstance(base, dict) and base.get("sha"):
                candidates.append(str(base["sha"]))
        if payload.get("before"):
            candidates.append(str(payload["before"]))

    candidates.extend(
        candidate
        for candidate in (
            os.environ.get("WH_CI_DIFF_BASE_REF"),
            f"origin/{os.environ['GITHUB_BASE_REF']}"
            if os.environ.get("GITHUB_BASE_REF")
            else None,
            os.environ.get("GITHUB_BASE_REF"),
            "origin/main",
            "main",
        )
        if candidate
    )

    for candidate in candidates:
        result = subprocess.run(
            ["git", "rev-parse", "--verify", "--quiet", f"{candidate}^{{commit}}"],
            cwd=str(ROOT),
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        if result.returncode == 0:
            return candidate
    return None


def git_listing(pathspecs: Sequence[str], *, base_ref: str | None = None) -> list[Path]:
    if base_ref:
        cmd = [
            "git",
            "diff",
            "--name-only",
            "--diff-filter=ACMR",
            f"{base_ref}...HEAD",
            "--",
            *pathspecs,
        ]
    else:
        cmd = ["git", "ls-files", *pathspecs]

    completed = run(cmd, capture_output=True)
    entries: list[Path] = []
    for line in completed.stdout.splitlines():
        rel = line.strip()
        if not rel:
            continue
        path = (ROOT / rel).resolve()
        if path.exists():
            entries.append(path)
    return entries


def collect_source_files(scope_mode: str) -> tuple[list[Path], str]:
    incremental_requested = scope_mode in {"auto", "changed", "diff", "incremental"}
    diff_base = resolve_diff_base() if incremental_requested else None

    if diff_base:
        files = git_listing(
            ["*.hpp", "*.h", "*.cpp", "*.cc", "*.cxx", "*.ipp"],
            base_ref=diff_base,
        )
        scope_label = f"incremental:{diff_base}"
    else:
        files = git_listing(["*.hpp", "*.h", "*.cpp", "*.cc", "*.cxx", "*.ipp"])
        scope_label = "full"

    filtered = [
        file
        for file in files
        if under(file, ROOT) and not under(file, THIRD_PARTY_ROOT) and not under(file, BUILD_ROOT)
    ]
    return filtered, scope_label


def collect_shell_files(scope_mode: str) -> tuple[list[Path], str]:
    incremental_requested = scope_mode in {"auto", "changed", "diff", "incremental"}
    diff_base = resolve_diff_base() if incremental_requested else None

    if diff_base:
        files = git_listing(["*.sh", "*.bash"], base_ref=diff_base)
        scope_label = f"incremental:{diff_base}"
    else:
        files = git_listing(["*.sh", "*.bash"])
        scope_label = "full"

    filtered = [
        file
        for file in files
        if under(file, ROOT) and not under(file, THIRD_PARTY_ROOT) and not under(file, BUILD_ROOT)
    ]
    return filtered, scope_label


def load_manifest(path: Path) -> list[TestEntry]:
    entries: list[TestEntry] = []
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle, delimiter="\t")
        for row in reader:
            labels = tuple(label for label in (row.get("labels") or "").split(",") if label)
            timeout_seconds = int(row.get("timeout_seconds") or 0)
            source_rel = row["source"]
            entries.append(
                TestEntry(
                    suite=row["suite"],
                    target=row["target"],
                    executable=Path(row["executable"]),
                    source=ROOT / source_rel,
                    source_size=max(1, int(row.get("source_size") or 1)),
                    weight=max(1, int(row.get("weight") or row.get("source_size") or 1)),
                    timeout_seconds=max(0, timeout_seconds),
                    labels=labels,
                )
            )
    return entries


def filter_test_entries(
    entries: Sequence[TestEntry],
    *,
    suites: Sequence[str],
    include_labels: Sequence[str],
    exclude_labels: Sequence[str],
) -> list[TestEntry]:
    suite_set = {suite for suite in suites if suite}
    include_set = {label for label in include_labels if label}
    exclude_set = {label for label in exclude_labels if label}

    filtered: list[TestEntry] = []
    for entry in entries:
        labels = set(entry.labels)
        if suite_set and entry.suite not in suite_set:
            continue
        if include_set and not include_set.issubset(labels):
            continue
        if exclude_set and exclude_set.intersection(labels):
            continue
        filtered.append(entry)
    return filtered


def load_matrix_lanes(path: Path) -> list[MatrixLane]:
    payload = json.loads(path.read_text())
    if not isinstance(payload, list):
        fail("matrix", f"matrix config must be a JSON array: {path}", code=2)

    lanes: list[MatrixLane] = []
    for index, item in enumerate(payload):
        if not isinstance(item, dict):
            fail("matrix", f"matrix entry #{index} must be a JSON object", code=2)

        name = str(item.get("name") or "")
        configure_preset = str(item.get("configure_preset") or "")
        if not name or not configure_preset:
            fail(
                "matrix",
                f"matrix entry #{index} requires name and configure_preset",
                code=2,
            )

        build_preset = str(item.get("build_preset") or configure_preset)
        min_shards = max(1, int(item.get("min_shards") or 1))
        suites = tuple(str(value) for value in (item.get("suites") or []))
        include_labels = tuple(str(value) for value in (item.get("include_labels") or []))
        exclude_labels = tuple(str(value) for value in (item.get("exclude_labels") or []))
        os_name = item.get("os")
        lanes.append(
            MatrixLane(
                name=name,
                configure_preset=configure_preset,
                build_preset=build_preset,
                min_shards=min_shards,
                suites=suites,
                include_labels=include_labels,
                exclude_labels=exclude_labels,
                os=str(os_name) if os_name else None,
            )
        )
    return lanes


def shard_test_entries(
    entries: Sequence[TestEntry], shard_count: int, shard_index: int
) -> list[TestEntry]:
    shards: list[list[TestEntry]] = [[] for _ in range(shard_count)]
    weights = [0 for _ in range(shard_count)]

    ordered = sorted(
        entries,
        key=lambda entry: (-entry.weight, -entry.source_size, entry.target, str(entry.source)),
    )
    for entry in ordered:
        target = min(
            range(shard_count),
            key=lambda index: (weights[index], len(shards[index]), index),
        )
        shards[target].append(entry)
        weights[target] += entry.weight
    return sorted(shards[shard_index], key=lambda entry: (entry.target, str(entry.source)))


def compute_test_shards(
    entries: Sequence[TestEntry],
    *,
    min_shards: int,
    max_build_actions_per_shard: int,
    cost_model: TestBuildCostModel,
) -> tuple[int, list[list[TestEntry]]]:
    if not entries:
        fail("matrix", "cannot compute shards for an empty entry set", code=2)
    if max_build_actions_per_shard <= 0:
        fail("matrix", "max_build_actions_per_shard must be positive", code=2)
    if cost_model.fixed_actions >= max_build_actions_per_shard:
        fail(
            "matrix",
            "fixed build actions already exceed the shard budget: "
            f"{cost_model.fixed_actions} >= {max_build_actions_per_shard}",
            code=2,
        )

    shard_count = max(1, min_shards)
    while True:
        shards = [shard_test_entries(entries, shard_count, index) for index in range(shard_count)]
        largest_actions = max(
            (cost_model.estimate_actions_for_targets(len(shard)) for shard in shards),
            default=0,
        )
        if largest_actions <= max_build_actions_per_shard:
            return shard_count, shards
        shard_count += 1


def manifest_shard(
    manifest_path: Path,
    *,
    shard_count: int,
    shard_index: int,
    suites: Sequence[str],
    include_labels: Sequence[str],
    exclude_labels: Sequence[str],
) -> list[TestEntry]:
    if not manifest_path.exists():
        fail("toolchain", f"missing test manifest: {manifest_path}", code=2)

    if shard_count <= 0 or shard_index < 0 or shard_index >= shard_count:
        fail("toolchain", "invalid shard selection", code=2)

    filtered_entries = filter_test_entries(
        load_manifest(manifest_path),
        suites=suites,
        include_labels=include_labels,
        exclude_labels=exclude_labels,
    )
    if not filtered_entries:
        fail("toolchain", "manifest filter selected no tests", code=2)

    return shard_test_entries(filtered_entries, shard_count, shard_index)


def run_test_entries(
    entries: Sequence[TestEntry],
    *,
    reporter: str,
    default_timeout_seconds: int,
    env: dict[str, str] | None = None,
) -> None:
    merged_env = os.environ.copy()
    if env:
        merged_env.update(env)

    for entry in entries:
        timeout_seconds = (
            entry.timeout_seconds if entry.timeout_seconds > 0 else default_timeout_seconds
        )
        print_step(
            "test-shard",
            "RUN "
            f"{entry.target} ({entry.suite}, weight={entry.weight}, timeout={timeout_seconds}s, "
            f"labels={','.join(entry.labels)})",
        )
        start = time.monotonic()
        try:
            completed = subprocess.run(
                [
                    str(entry.executable),
                    "--reporter",
                    reporter,
                    "--allow-running-no-tests",
                ],
                cwd=str(ROOT),
                env=merged_env,
                check=False,
                text=True,
                timeout=timeout_seconds,
            )
        except FileNotFoundError as error:
            fail("test-shard", f"failed to launch {entry.target}: {error}", code=127)
        except OSError as error:
            fail("test-shard", f"failed to launch {entry.target}: {error}", code=127)
        except subprocess.TimeoutExpired:
            fail("test-shard", f"target {entry.target} timed out after {timeout_seconds}s", code=124)

        elapsed = time.monotonic() - start
        print_step("test-shard", f"DONE {entry.target} elapsed={elapsed:.2f}s")
        if completed.returncode != 0:
            print_step("test-shard", f"FAIL {entry.target} exited with {completed.returncode}")
            raise SystemExit(completed.returncode)

    print_step("test-shard", f"PASS {len(entries)} test targets")


def filter_compile_commands_to_project_scope(
    compile_db: Path, target_file: Path, *, include_tests: bool = True
) -> None:
    entries = json.loads(compile_db.read_text())
    filtered: list[dict[str, object]] = []
    for entry in entries:
        file_path = resolve_compile_entry_file(entry)
        if file_path is None:
            continue
        if not is_project_path_in_scope(file_path, include_tests=include_tests):
            continue
        filtered.append(entry)

    if not filtered:
        fail("compile-commands", "no project translation units after filtering")

    target_file.write_text(json.dumps(filtered, indent=2) + "\n")
    print_step("compile-commands", f"FILTER {len(filtered)}/{len(entries)} -> {target_file}")


def load_compile_entries(
    compile_db: Path, *, include_tests: bool = True, include_third_party: bool = False
) -> list[CompileEntry]:
    raw_entries = json.loads(compile_db.read_text())
    dedup: dict[Path, CompileEntry] = {}
    for entry in raw_entries:
        file_path = resolve_compile_entry_file(entry)
        if file_path is None:
            continue

        if not is_project_path_in_scope(
            file_path,
            include_tests=include_tests,
            include_third_party=include_third_party,
        ):
            continue

        dedup[file_path] = CompileEntry(file=file_path, size=max(1, file_path.stat().st_size))
    return list(dedup.values())


def count_link_actions(build_ninja: Path) -> int:
    if not build_ninja.exists():
        fail("matrix", f"build graph missing: {build_ninja}", code=2)

    linker_rules = (
        "CXX_EXECUTABLE_LINKER",
        "CXX_STATIC_LIBRARY_LINKER",
        "CXX_SHARED_LIBRARY_LINKER",
        "CXX_MODULE_LIBRARY_LINKER",
    )
    count = 0
    with build_ninja.open("r", encoding="utf-8") as handle:
        for line in handle:
            if not line.startswith("build "):
                continue
            if any(f": {rule}" in line for rule in linker_rules):
                count += 1
    return count


def build_cost_model_for_manifest(
    preset: str, manifest_entries: Sequence[TestEntry]
) -> TestBuildCostModel:
    build_dir = build_dir_for_preset(preset)
    compile_db = build_dir / "compile_commands.json"
    all_compile_entries = load_compile_entries(
        compile_db,
        include_tests=True,
        include_third_party=True,
    )
    if not all_compile_entries:
        fail("matrix", f"no compile entries found for preset {preset}", code=2)

    compile_paths = {entry.file.resolve() for entry in all_compile_entries}
    manifest_sources = {entry.source.resolve() for entry in manifest_entries}
    missing_sources = sorted(path for path in manifest_sources if path not in compile_paths)
    if missing_sources:
        missing_preview = ", ".join(str(path.relative_to(ROOT)) for path in missing_sources[:3])
        fail(
            "matrix",
            "build-action planning requires source-layout tests; "
            f"manifest sources missing from compile database for preset {preset}: {missing_preview}",
            code=2,
        )

    compile_total = len(all_compile_entries)
    manifest_target_count = len(manifest_entries)
    manifest_compile_count = len(manifest_sources)
    fixed_compile_actions = compile_total - manifest_compile_count
    fixed_link_actions = count_link_actions(build_dir / "build.ninja") - manifest_target_count
    if fixed_compile_actions < 0 or fixed_link_actions < 0:
        fail(
            "matrix",
            f"invalid build-action model for preset {preset}: "
            f"fixed_compile={fixed_compile_actions}, fixed_link={fixed_link_actions}",
            code=2,
        )

    return TestBuildCostModel(
        fixed_compile_actions=fixed_compile_actions,
        fixed_link_actions=fixed_link_actions,
    )


def recompute_coverage_totals(
    files: Sequence[dict[str, object]],
) -> dict[str, dict[str, int | float]]:
    totals: dict[str, dict[str, int | float]] = {}
    metric_names = ("branches", "functions", "instantiations", "lines", "mcdc", "regions")
    for metric_name in metric_names:
        count = 0
        covered = 0
        notcovered = 0
        has_notcovered = False
        for file_payload in files:
            summary = file_payload.get("summary")
            if not isinstance(summary, dict):
                continue
            metric = summary.get(metric_name)
            if not isinstance(metric, dict):
                continue
            count += int(metric.get("count", 0))
            covered += int(metric.get("covered", 0))
            if "notcovered" in metric:
                notcovered += int(metric.get("notcovered", 0))
                has_notcovered = True

        metric_totals: dict[str, int | float] = {
            "count": count,
            "covered": covered,
            "percent": (float(covered) * 100.0 / float(count)) if count else 0.0,
        }
        if has_notcovered:
            metric_totals["notcovered"] = notcovered
        totals[metric_name] = metric_totals
    return totals


def filter_coverage_report_to_project_scope(
    report_json: Path, *, include_tests: bool = True
) -> dict[str, object]:
    payload = json.loads(report_json.read_text())
    data = payload.get("data")
    if not isinstance(data, list):
        fail("coverage", f"coverage export did not contain data array: {report_json}")

    filtered_data: list[dict[str, object]] = []
    total_files_before = 0
    total_files_after = 0

    for datum in data:
        if not isinstance(datum, dict):
            continue

        raw_files = datum.get("files")
        if not isinstance(raw_files, list):
            raw_files = []
        total_files_before += len(raw_files)

        filtered_files: list[dict[str, object]] = []
        for file_payload in raw_files:
            if not isinstance(file_payload, dict):
                continue
            filename = file_payload.get("filename")
            if not filename:
                continue
            file_path = Path(str(filename)).resolve()
            if not is_project_path_in_scope(file_path, include_tests=include_tests):
                continue
            filtered_files.append(file_payload)

        total_files_after += len(filtered_files)
        filtered_datum = dict(datum)
        filtered_datum["files"] = filtered_files
        filtered_datum["totals"] = recompute_coverage_totals(filtered_files)
        filtered_data.append(filtered_datum)

    if total_files_after == 0:
        fail("coverage", "coverage export contained no in-scope project files")

    payload["data"] = filtered_data
    report_json.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print_step("coverage", f"FILTER files {total_files_after}/{total_files_before}")
    return payload


def coverage_tools() -> tuple[str, str]:
    llvm_cov_bin = os.environ.get("WH_LLVM_COV_BIN", "llvm-cov")
    llvm_profdata_bin = os.environ.get("WH_LLVM_PROFDATA_BIN", "llvm-profdata")
    require_commands("coverage", "cmake", llvm_cov_bin, llvm_profdata_bin)
    return llvm_cov_bin, llvm_profdata_bin


def merge_coverage_profiles(
    tag: str,
    llvm_profdata_bin: str,
    profile_inputs: Sequence[Path],
    output_file: Path,
) -> None:
    if not profile_inputs:
        fail(tag, "no coverage profiles to merge")
    output_file.parent.mkdir(parents=True, exist_ok=True)
    run(
        [
            llvm_profdata_bin,
            "merge",
            "-sparse",
            *[str(path) for path in profile_inputs],
            "-o",
            str(output_file),
        ]
    )
    print_step(tag, f"MERGED profiles={len(profile_inputs)} -> {output_file}")


def export_coverage_summary(
    tag: str,
    llvm_cov_bin: str,
    profdata_file: Path,
    binaries: Sequence[str],
    report_json: Path,
) -> None:
    report_json.parent.mkdir(parents=True, exist_ok=True)
    export_result = run(
        [
            llvm_cov_bin,
            "export",
            "--summary-only",
            "--instr-profile",
            str(profdata_file),
            *binaries,
        ],
        check=False,
        capture_output=True,
    )
    if export_result.stdout:
        report_json.write_text(export_result.stdout, encoding="utf-8")
    else:
        report_json.write_text("", encoding="utf-8")
    if export_result.stderr:
        sys.stderr.write(export_result.stderr)
    if export_result.returncode != 0:
        fail(tag, f"llvm-cov export failed with status {export_result.returncode}")
    if "mismatched data" in export_result.stderr:
        fail(
            tag,
            "llvm-cov reported mismatched data across coverage binaries; "
            "aggregate line coverage is unreliable",
        )


def check_coverage_gate(tag: str, report_json: Path, minimum_lines: float) -> None:
    payload = filter_coverage_report_to_project_scope(
        report_json,
        include_tests=True,
    )
    data = payload.get("data") or []
    totals = data[0].get("totals", {}) if data else {}
    lines = totals.get("lines", {}) if isinstance(totals, dict) else {}
    percent = lines.get("percent")
    if percent is None:
        fail(tag, "coverage export did not contain line percentage")

    line_pct = float(percent)
    min_pct = float(minimum_lines) * 100.0
    if line_pct + 1e-9 < min_pct:
        fail(tag, f"line coverage {line_pct:.2f}% below threshold {min_pct:.2f}%")
    print_step(tag, f"PASS line coverage {line_pct:.2f}% (threshold {min_pct:.2f}%)")


def collect_coverage_profiles(tag: str, profile_dir: Path) -> list[Path]:
    if not profile_dir.exists():
        fail(tag, f"missing coverage profile directory: {profile_dir}")

    profdata_files = sorted(path for path in profile_dir.rglob("*.profdata") if path.is_file())
    if profdata_files:
        return profdata_files

    profraw_files = sorted(path for path in profile_dir.rglob("*.profraw") if path.is_file())
    if profraw_files:
        return profraw_files

    fail(tag, f"no coverage profiles found under {profile_dir}")


def collect_coverage_binaries(tag: str, binaries_dir: Path) -> list[Path]:
    if not binaries_dir.exists():
        fail(tag, f"missing coverage binaries directory: {binaries_dir}")

    binaries = sorted(path for path in binaries_dir.rglob("*") if path.is_file())
    if not binaries:
        fail(tag, f"no coverage binaries found under {binaries_dir}")
    return binaries


def shard_compile_entries(
    entries: Sequence[CompileEntry], shard_count: int, shard_index: int
) -> list[CompileEntry]:
    shards: list[list[CompileEntry]] = [[] for _ in range(shard_count)]
    weights = [0 for _ in range(shard_count)]
    for entry in sorted(entries, key=lambda item: (-item.size, str(item.file))):
        target = min(
            range(shard_count),
            key=lambda index: (weights[index], len(shards[index]), index),
        )
        shards[target].append(entry)
        weights[target] += entry.size
    return sorted(shards[shard_index], key=lambda item: str(item.file))


def configure_and_validate_manifest(
    preset: str,
    tag: str,
    *,
    cache_entries: Sequence[str] = (),
) -> Path:
    build_dir = configure_preset(preset, tag, cache_entries=cache_entries)
    manifest_path = build_dir / "wh_test_manifest.tsv"
    if not manifest_path.exists():
        fail(tag, f"missing test manifest: {manifest_path}")
    return manifest_path


def ci_fast_gates(args: argparse.Namespace) -> None:
    require_commands("fast-gates", "git", "shellcheck")

    formatter_bin = os.environ.get("WH_CLANG_FORMAT_BIN") or shutil.which("clang-format")
    if formatter_bin is None:
        fail("clang-format", "clang-format not installed")

    shell_files, shell_scope = collect_shell_files(args.shellcheck_scope)
    if shell_files:
        print_step("shellcheck", f"scope: {shell_scope}")
        run(["shellcheck", "-x", *[str(path.relative_to(ROOT)) for path in shell_files]])
        print_step("shellcheck", "PASS")
    else:
        print_step("shellcheck", f"SKIP no shell scripts in scope ({shell_scope})")

    source_files, source_scope = collect_source_files(args.clang_format_scope)
    if not source_files:
        print_step("clang-format", f"SKIP no source files in scope ({source_scope})")
        return

    print_step("clang-format", f"scope: {source_scope}")
    run(
        [
            formatter_bin,
            "--style=file",
            "--fallback-style=none",
            "--dry-run",
            "--Werror",
            *[str(path.relative_to(ROOT)) for path in source_files],
        ]
    )
    print_step("clang-format", "PASS")


def run_build_test_mode(
    tag: str,
    *,
    configure_preset_name: str,
    build_preset_name: str,
    shard_count: int,
    shard_index: int,
    suites: Sequence[str],
    include_labels: Sequence[str],
    exclude_labels: Sequence[str],
    default_timeout_seconds: int,
    reporter: str,
    env: dict[str, str] | None = None,
    cache_entries: Sequence[str] = (),
) -> None:
    manifest_path = configure_and_validate_manifest(
        configure_preset_name,
        tag,
        cache_entries=cache_entries,
    )
    shard = manifest_shard(
        manifest_path,
        shard_count=shard_count,
        shard_index=shard_index,
        suites=suites,
        include_labels=include_labels,
        exclude_labels=exclude_labels,
    )
    if not shard:
        print_step(tag, f"SKIP empty shard {shard_index}/{shard_count}")
        return

    targets = [entry.target for entry in shard]
    print_step(
        tag,
        f"preset={configure_preset_name} shard={shard_index}/{shard_count} targets={len(targets)}",
    )
    build_targets_in_batches(build_preset_name, tag, targets=targets)
    print_compiler_cache_stats()
    run_test_entries(
        shard,
        reporter=reporter,
        default_timeout_seconds=default_timeout_seconds,
        env=env,
    )
    print_step(tag, "PASS")


def ci_emit_test_matrix(args: argparse.Namespace) -> None:
    config_path = args.config.resolve()
    if not config_path.exists():
        fail("matrix", f"missing config: {config_path}", code=2)

    lanes = load_matrix_lanes(config_path)
    manifest_cache: dict[str, list[TestEntry]] = {}
    cost_model_cache: dict[str, TestBuildCostModel] = {}
    matrix_include: list[dict[str, object]] = []

    for lane in lanes:
        if lane.configure_preset not in manifest_cache:
            manifest_path = configure_and_validate_manifest(lane.configure_preset, "matrix")
            manifest_cache[lane.configure_preset] = load_manifest(manifest_path)
            cost_model_cache[lane.configure_preset] = build_cost_model_for_manifest(
                lane.configure_preset,
                manifest_cache[lane.configure_preset],
            )

        filtered_entries = filter_test_entries(
            manifest_cache[lane.configure_preset],
            suites=lane.suites,
            include_labels=lane.include_labels,
            exclude_labels=lane.exclude_labels,
        )
        if not filtered_entries:
            fail(
                "matrix",
                f"lane {lane.name} selected no tests for preset {lane.configure_preset}",
                code=2,
            )

        shard_count, shards = compute_test_shards(
            filtered_entries,
            min_shards=lane.min_shards,
            max_build_actions_per_shard=args.max_build_actions_per_shard,
            cost_model=cost_model_cache[lane.configure_preset],
        )
        largest_shard = max(len(shard) for shard in shards)
        largest_actions = max(
            cost_model_cache[lane.configure_preset].estimate_actions_for_targets(len(shard))
            for shard in shards
        )
        print_step(
            "matrix",
            f"{lane.name}: targets={len(filtered_entries)} resolved_shards={shard_count} "
            f"largest_shard={largest_shard} estimated_actions={largest_actions} "
            f"limit={args.max_build_actions_per_shard}",
        )

        for shard_index, shard in enumerate(shards):
            estimated_build_actions = cost_model_cache[
                lane.configure_preset
            ].estimate_actions_for_targets(len(shard))
            payload: dict[str, object] = {
                "name": lane.name,
                "configure_preset": lane.configure_preset,
                "build_preset": lane.build_preset,
                "shard_count": shard_count,
                "shard_index": shard_index,
                "target_count": len(shard),
                "total_weight": sum(entry.weight for entry in shard),
                "estimated_build_actions": estimated_build_actions,
            }
            if lane.os:
                payload["os"] = lane.os
            matrix_include.append(payload)

    output_payload = {"include": matrix_include}
    encoded = json.dumps(output_payload, separators=(",", ":"))
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded + "\n", encoding="utf-8")
        print_step("matrix", f"WROTE {args.output}")
        return

    print(encoded)


def ci_build_test(args: argparse.Namespace) -> None:
    run_build_test_mode(
        "build-test",
        configure_preset_name=args.configure_preset,
        build_preset_name=args.build_preset or args.configure_preset,
        shard_count=args.shard_count,
        shard_index=args.shard_index,
        suites=args.suite,
        include_labels=args.include_label,
        exclude_labels=args.exclude_label,
        default_timeout_seconds=args.default_timeout_seconds,
        reporter=args.reporter,
    )


def probe_lsan_runtime() -> tuple[bool, str]:
    if sys.platform == "darwin":
        return False, "disabled on darwin runners"

    if shutil.which("ps") is None:
        return False, "disabled: ps command not found"

    completed = subprocess.run(
        ["/bin/ps", "-p", str(os.getpid()), "-o", "pid="],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    if completed.returncode != 0:
        return False, "disabled: ps exists but execution is not permitted"
    return True, "enabled"


def ci_sanitizer(args: argparse.Namespace) -> None:
    lsan_mode = args.lsan_mode
    detect_leaks = "1"
    lsan_reason = "enabled"
    if lsan_mode == "off":
        detect_leaks = "0"
        lsan_reason = "forced off by --lsan-mode off"
    elif lsan_mode == "on":
        detect_leaks = "1"
        lsan_reason = "forced on by --lsan-mode on"
    elif lsan_mode == "auto":
        enabled, lsan_reason = probe_lsan_runtime()
        detect_leaks = "1" if enabled else "0"
    else:
        fail("sanitizer", f"invalid lsan mode: {lsan_mode}", code=2)

    env = {
        "ASAN_OPTIONS": os.environ.get("ASAN_OPTIONS", f"detect_leaks={detect_leaks}"),
        "UBSAN_OPTIONS": os.environ.get("UBSAN_OPTIONS", "print_stacktrace=1"),
    }
    print_step("sanitizer", f"lsan_mode={lsan_mode} ({lsan_reason})")
    print_step("sanitizer", f"ASAN_OPTIONS={env['ASAN_OPTIONS']}")
    print_step("sanitizer", f"UBSAN_OPTIONS={env['UBSAN_OPTIONS']}")

    run_build_test_mode(
        "sanitizer",
        configure_preset_name=args.configure_preset,
        build_preset_name=args.build_preset or args.configure_preset,
        shard_count=args.shard_count,
        shard_index=args.shard_index,
        suites=args.suite,
        include_labels=args.include_label or ["sanitizer.safe"],
        exclude_labels=args.exclude_label or ["ci.nightly"],
        default_timeout_seconds=args.default_timeout_seconds,
        reporter=args.reporter,
        env=env,
    )


def run_clang_tidy_file(
    clang_tidy_bin: str,
    build_dir: Path,
    source_file: Path,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [clang_tidy_bin, "-p", str(build_dir), "--quiet", str(source_file)],
        cwd=str(ROOT),
        check=False,
        text=True,
        capture_output=True,
    )


def ci_clang_tidy(args: argparse.Namespace) -> None:
    if os.environ.get("RUNNER_OS") in {"Windows", "macOS"}:
        print_step("clang-tidy", f"SKIP {os.environ['RUNNER_OS'].lower()} runner")
        return

    clang_tidy_bin = os.environ.get("WH_CLANG_TIDY_BIN", "clang-tidy")
    require_commands("clang-tidy", "cmake", clang_tidy_bin)

    build_dir = configure_preset(args.configure_preset, "clang-tidy")
    compile_db = build_dir / "compile_commands.json"
    if not compile_db.exists():
        fail("clang-tidy", f"compile_commands.json missing: {compile_db}")

    entries = load_compile_entries(compile_db, include_tests=False)
    if not entries:
        fail("clang-tidy", "no first-party translation units found")
    if args.shard_count <= 0 or args.shard_index < 0 or args.shard_index >= args.shard_count:
        fail("clang-tidy", "invalid shard selection", code=2)

    shard = shard_compile_entries(entries, args.shard_count, args.shard_index)
    if not shard:
        print_step("clang-tidy", f"SKIP empty shard {args.shard_index}/{args.shard_count}")
        return

    source_files = [entry.file for entry in shard]
    jobs = max(1, int(args.jobs))
    print_step("clang-tidy", f"preset: {args.configure_preset}")
    print_step("clang-tidy", f"shard: {args.shard_index}/{args.shard_count}")
    print_step("clang-tidy", f"files: {len(source_files)}")
    print_step("clang-tidy", f"jobs: {jobs}")

    failures: list[Path] = []
    if jobs == 1 or len(source_files) == 1:
        for source_file in source_files:
            completed = run_clang_tidy_file(clang_tidy_bin, build_dir, source_file)
            if completed.stdout:
                sys.stdout.write(completed.stdout)
            if completed.stderr:
                sys.stderr.write(completed.stderr)
            if completed.returncode != 0:
                failures.append(source_file)
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
            future_map = {
                executor.submit(run_clang_tidy_file, clang_tidy_bin, build_dir, source_file): source_file
                for source_file in source_files
            }
            for future in concurrent.futures.as_completed(future_map):
                source_file = future_map[future]
                completed = future.result()
                if completed.stdout:
                    sys.stdout.write(completed.stdout)
                if completed.stderr:
                    sys.stderr.write(completed.stderr)
                if completed.returncode != 0:
                    failures.append(source_file)

    if failures:
        fail(
            "clang-tidy",
            "failed files: " + ", ".join(str(path.relative_to(ROOT)) for path in sorted(failures)),
        )
    print_step("clang-tidy", "PASS")


def ci_codechecker(args: argparse.Namespace) -> None:
    if os.environ.get("RUNNER_OS") in {"Windows", "macOS"}:
        print_step("codechecker", f"SKIP {os.environ['RUNNER_OS'].lower()} runner")
        return

    analyzers = ("clangsa",)
    codechecker_bin = os.environ.get("WH_CODECHECKER_BIN", "CodeChecker")
    if shutil.which(codechecker_bin) is None and shutil.which("codechecker") is not None:
        codechecker_bin = "codechecker"
    require_commands("codechecker", "cmake", codechecker_bin)

    build_dir = configure_preset(args.configure_preset, "codechecker")
    compile_db = build_dir / "compile_commands.json"
    if not compile_db.exists():
        fail("codechecker", f"compile_commands.json missing: {compile_db}")

    filtered_compile_db = build_dir / "compile_commands.codechecker.json"
    report_dir = build_dir / "codechecker-reports"
    report_json = build_dir / "codechecker-reports.json"
    filter_compile_commands_to_project_scope(
        compile_db,
        filtered_compile_db,
        include_tests=False,
    )
    if report_dir.exists():
        shutil.rmtree(report_dir)
    if report_json.exists():
        report_json.unlink()

    print_step("codechecker", f"preset: {args.configure_preset}")
    print_step("codechecker", f"analyzers: {', '.join(analyzers)}")
    run(
        [
            codechecker_bin,
            "analyze",
            str(filtered_compile_db),
            "--output",
            str(report_dir),
            "--analyzers",
            *analyzers,
            "--jobs",
            str(args.jobs),
            "--clean",
        ]
    )

    # CodeChecker may report missing results for analyzers we did not request.
    # The exported JSON report is the reliable contract for this CI gate.
    parse_result = run(
        [codechecker_bin, "parse", str(report_dir), "-e", "json", "-o", str(report_json)],
        check=False,
        capture_output=True,
    )
    print_process_output(parse_result)
    if not report_json.exists():
        fail("codechecker", f"parse did not produce JSON report: {report_json}")

    payload = json.loads(report_json.read_text())
    issue_count = len(payload.get("reports", []))
    if issue_count != 0:
        subprocess.run([codechecker_bin, "parse", str(report_dir)], check=False)
        fail("codechecker", f"findings={issue_count}")
    if parse_result.returncode != 0:
        print_step(
            "codechecker",
            f"parse exited with status {parse_result.returncode}; using exported JSON findings",
        )
    print_step("codechecker", "PASS")


def ci_coverage(args: argparse.Namespace) -> None:
    llvm_cov_bin, llvm_profdata_bin = coverage_tools()

    manifest_path = configure_and_validate_manifest(args.configure_preset, "coverage")
    build_dir = build_dir_for_preset(args.configure_preset)
    shard = manifest_shard(
        manifest_path,
        shard_count=1,
        shard_index=0,
        suites=args.suite,
        include_labels=args.include_label or ["ci.pr"],
        exclude_labels=args.exclude_label,
    )
    if not shard:
        fail("coverage", "coverage run selected no tests")

    targets = [entry.target for entry in shard]
    build_targets_in_batches(
        args.build_preset or args.configure_preset,
        "coverage",
        targets=targets,
    )
    print_compiler_cache_stats()

    profile_dir = build_dir / "profiles"
    report_dir = build_dir / "reports"
    profdata_file = report_dir / "coverage.profdata"
    report_json = report_dir / "coverage-summary.json"
    profile_dir.mkdir(parents=True, exist_ok=True)
    report_dir.mkdir(parents=True, exist_ok=True)

    for file in profile_dir.glob("*.profraw"):
        file.unlink()
    if profdata_file.exists():
        profdata_file.unlink()

    run_test_entries(
        shard,
        reporter=args.reporter,
        default_timeout_seconds=args.default_timeout_seconds,
        env={"LLVM_PROFILE_FILE": str(profile_dir / "%m-%p.profraw")},
    )

    profraw_files = collect_coverage_profiles("coverage", profile_dir)
    merge_coverage_profiles("coverage", llvm_profdata_bin, profraw_files, profdata_file)

    binaries = [str(entry.executable) for entry in shard]
    export_coverage_summary("coverage", llvm_cov_bin, profdata_file, binaries, report_json)
    check_coverage_gate("coverage", report_json, args.coverage_min_lines)


def ci_coverage_shard(args: argparse.Namespace) -> None:
    _, llvm_profdata_bin = coverage_tools()
    if args.profile_output is None and args.artifact_dir is None:
        fail("coverage-shard", "either --profile-output or --artifact-dir is required", code=2)

    manifest_path = configure_and_validate_manifest(args.configure_preset, "coverage-shard")
    build_dir = build_dir_for_preset(args.configure_preset)
    shard = manifest_shard(
        manifest_path,
        shard_count=args.shard_count,
        shard_index=args.shard_index,
        suites=args.suite,
        include_labels=args.include_label or ["ci.pr"],
        exclude_labels=args.exclude_label,
    )
    if not shard:
        fail("coverage-shard", "coverage shard selected no tests")

    targets = [entry.target for entry in shard]
    print_step(
        "coverage-shard",
        f"preset={args.configure_preset} shard={args.shard_index}/{args.shard_count} "
        f"targets={len(targets)}",
    )
    build_targets_in_batches(
        args.build_preset or args.configure_preset,
        "coverage-shard",
        targets=targets,
    )
    print_compiler_cache_stats()

    profile_dir = build_dir / "profiles" / f"shard-{args.shard_index}"
    if profile_dir.exists():
        shutil.rmtree(profile_dir)
    profile_dir.mkdir(parents=True, exist_ok=True)

    run_test_entries(
        shard,
        reporter=args.reporter,
        default_timeout_seconds=args.default_timeout_seconds,
        env={"LLVM_PROFILE_FILE": str(profile_dir / "%m-%p.profraw")},
    )

    profile_output = (
        args.profile_output.resolve()
        if args.profile_output is not None
        else (build_dir / "reports" / f"coverage-shard-{args.shard_index}.profdata")
    )
    merge_coverage_profiles(
        "coverage-shard",
        llvm_profdata_bin,
        collect_coverage_profiles("coverage-shard", profile_dir),
        profile_output,
    )
    if args.artifact_dir is not None:
        artifact_dir = args.artifact_dir.resolve()
        if artifact_dir.exists():
            shutil.rmtree(artifact_dir)
        profiles_dir = artifact_dir / "profiles"
        binaries_dir = artifact_dir / "binaries"
        profiles_dir.mkdir(parents=True, exist_ok=True)
        binaries_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(
            profile_output,
            profiles_dir / f"coverage-shard-{args.shard_index}.profdata",
        )
        for entry in shard:
            shutil.copy2(entry.executable, binaries_dir / entry.target)
        print_step("coverage-shard", f"ARTIFACT {artifact_dir}")
    print_step("coverage-shard", f"PASS {profile_output}")


def ci_coverage_aggregate(args: argparse.Namespace) -> None:
    llvm_cov_bin, llvm_profdata_bin = coverage_tools()
    artifact_dir = args.artifact_dir.resolve()
    profile_inputs = collect_coverage_profiles("coverage", artifact_dir / "profiles")
    binaries = [str(path) for path in collect_coverage_binaries("coverage", artifact_dir / "binaries")]
    report_dir = artifact_dir / "reports"
    profdata_file = report_dir / "coverage.profdata"
    report_json = report_dir / "coverage-summary.json"

    merge_coverage_profiles("coverage", llvm_profdata_bin, profile_inputs, profdata_file)
    export_coverage_summary("coverage", llvm_cov_bin, profdata_file, binaries, report_json)
    check_coverage_gate("coverage", report_json, args.coverage_min_lines)


def local_sync_third_party(_: argparse.Namespace) -> None:
    require_commands("sync-third-party", "git")
    run(["git", "submodule", "sync", "--recursive"])
    run(["git", "submodule", "update", "--init", "--recursive"])
    print_step("sync-third-party", "PASS")


def local_clean(args: argparse.Namespace) -> None:
    target = BUILD_ROOT if args.all else build_dir_for_preset(args.preset)
    if target.exists():
        shutil.rmtree(target)
        print_step("clean", f"REMOVED {target}")
    else:
        print_step("clean", f"SKIP missing {target}")


def local_configure(args: argparse.Namespace) -> None:
    build_dir = configure_preset(args.preset, "configure", cache_entries=args.define)
    if args.preset != DEFAULT_EDITOR_PRESET:
        ensure_editor_compile_database("configure")
    print_step("configure", f"PASS {build_dir}")


def local_build(args: argparse.Namespace) -> None:
    configure_preset(args.preset, "build", cache_entries=args.define)
    if args.preset != DEFAULT_EDITOR_PRESET:
        ensure_editor_compile_database("build")
    targets = list(args.target) if args.target else [build_artifact_target(args.artifacts)]
    build_preset(args.preset, "build", targets=targets, jobs=args.jobs)
    print_compiler_cache_stats()
    print_step("build", f"PASS {build_dir_for_preset(args.preset)}")


def local_test(args: argparse.Namespace) -> None:
    if args.build_first:
        local_build(
            argparse.Namespace(
                preset=args.preset,
                target=[],
                artifacts="tests",
                jobs=args.jobs,
                define=args.define,
            )
        )
    else:
        configure_preset(args.preset, "test", cache_entries=args.define)
        if args.preset != DEFAULT_EDITOR_PRESET:
            ensure_editor_compile_database("test")

    manifest_path = build_dir_for_preset(args.preset) / "wh_test_manifest.tsv"
    shard = manifest_shard(
        manifest_path,
        shard_count=args.shard_count,
        shard_index=args.shard_index,
        suites=args.suite,
        include_labels=args.include_label,
        exclude_labels=args.exclude_label,
    )
    if not shard:
        print_step("test", f"SKIP empty shard {args.shard_index}/{args.shard_count}")
        return

    run_test_entries(
        shard,
        reporter=args.reporter,
        default_timeout_seconds=args.default_timeout_seconds,
    )
    print_step("test", f"PASS {build_dir_for_preset(args.preset)}")


def local_verify(args: argparse.Namespace) -> None:
    if args.configure_preset != DEFAULT_EDITOR_PRESET:
        ensure_editor_compile_database("verify")
    run_build_test_mode(
        "verify",
        configure_preset_name=args.configure_preset,
        build_preset_name=args.build_preset or args.configure_preset,
        shard_count=args.shard_count,
        shard_index=args.shard_index,
        suites=args.suite,
        include_labels=args.include_label,
        exclude_labels=args.exclude_label,
        default_timeout_seconds=args.default_timeout_seconds,
        reporter=args.reporter,
        cache_entries=args.define,
    )


def local_editor(_: argparse.Namespace) -> None:
    ensure_editor_compile_database("editor")


def add_define_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--define",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="Append a CMake cache entry passed as -DKEY=VALUE during configure.",
    )


def add_shard_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--shard-count", type=int, default=1)
    parser.add_argument("--shard-index", type=int, default=0)
    parser.add_argument("--suite", action="append", default=[])
    parser.add_argument("--include-label", action="append", default=[])
    parser.add_argument("--exclude-label", action="append", default=[])
    parser.add_argument("--reporter", default=DEFAULT_REPORTER)
    parser.add_argument("--default-timeout-seconds", type=int, default=180)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Unified local and CI toolchain entrypoint.")
    subparsers = parser.add_subparsers(dest="domain", required=True)

    local = subparsers.add_parser("local", help="Local configure/build/test entrypoints.")
    local_sub = local.add_subparsers(dest="local_mode", required=True)

    local_sync_parser = local_sub.add_parser(
        "sync-third-party",
        help="Sync in-tree third-party submodules.",
    )
    local_sync_parser.set_defaults(func=local_sync_third_party)

    local_clean_parser = local_sub.add_parser("clean")
    local_clean_parser.add_argument("--preset", default=DEFAULT_LOCAL_PRESET)
    local_clean_parser.add_argument("--all", action="store_true")
    local_clean_parser.set_defaults(func=local_clean)

    local_editor_parser = local_sub.add_parser(
        "editor",
        help="Configure the full editor/clangd preset and refresh the root compile database.",
    )
    local_editor_parser.set_defaults(func=local_editor)

    local_configure_parser = local_sub.add_parser("configure")
    local_configure_parser.add_argument("--preset", default=DEFAULT_LOCAL_PRESET)
    add_define_args(local_configure_parser)
    local_configure_parser.set_defaults(func=local_configure)

    local_build_parser = local_sub.add_parser("build")
    local_build_parser.add_argument("--preset", default=DEFAULT_LOCAL_PRESET)
    local_build_parser.add_argument("--target", action="append", default=[])
    local_build_parser.add_argument("--artifacts", choices=("enabled", "tests"), default="enabled")
    local_build_parser.add_argument("--jobs")
    add_define_args(local_build_parser)
    local_build_parser.set_defaults(func=local_build)

    local_test_parser = local_sub.add_parser("test")
    local_test_parser.add_argument("--preset", default=DEFAULT_LOCAL_PRESET)
    local_test_parser.add_argument("--build-first", action="store_true")
    local_test_parser.add_argument("--jobs")
    add_define_args(local_test_parser)
    add_shard_args(local_test_parser)
    local_test_parser.set_defaults(func=local_test)

    local_verify_parser = local_sub.add_parser("verify")
    local_verify_parser.add_argument("--configure-preset", default=DEFAULT_LOCAL_PRESET)
    local_verify_parser.add_argument("--build-preset")
    add_define_args(local_verify_parser)
    add_shard_args(local_verify_parser)
    local_verify_parser.set_defaults(func=local_verify)

    ci = subparsers.add_parser("ci", help="CI orchestration entrypoints.")
    ci_sub = ci.add_subparsers(dest="ci_mode", required=True)

    fast_gates_parser = ci_sub.add_parser("fast-gates")
    fast_gates_parser.add_argument("--clang-format-scope", default="auto")
    fast_gates_parser.add_argument("--shellcheck-scope", default="auto")
    fast_gates_parser.set_defaults(func=ci_fast_gates)

    emit_matrix_parser = ci_sub.add_parser("emit-test-matrix")
    emit_matrix_parser.add_argument("--config", type=Path, required=True)
    emit_matrix_parser.add_argument("--output", type=Path)
    emit_matrix_parser.add_argument(
        "--max-build-actions-per-shard",
        type=int,
        default=int(os.environ.get("CI_MAX_BUILD_ACTIONS_PER_SHARD", "200")),
    )
    emit_matrix_parser.set_defaults(func=ci_emit_test_matrix)

    build_test_parser = ci_sub.add_parser("build-test")
    build_test_parser.add_argument("--configure-preset", required=True)
    build_test_parser.add_argument("--build-preset")
    add_shard_args(build_test_parser)
    build_test_parser.set_defaults(func=ci_build_test)

    sanitizer_parser = ci_sub.add_parser("sanitizer")
    sanitizer_parser.add_argument("--configure-preset", default=DEFAULT_SANITIZER_PRESET)
    sanitizer_parser.add_argument("--build-preset")
    sanitizer_parser.add_argument("--lsan-mode", choices=("auto", "on", "off"), default="auto")
    add_shard_args(sanitizer_parser)
    sanitizer_parser.set_defaults(func=ci_sanitizer)

    clang_tidy_parser = ci_sub.add_parser("clang-tidy")
    clang_tidy_parser.add_argument("--configure-preset", default=DEFAULT_ANALYSIS_PRESET)
    clang_tidy_parser.add_argument("--shard-count", type=int, default=1)
    clang_tidy_parser.add_argument("--shard-index", type=int, default=0)
    clang_tidy_parser.add_argument("--jobs", default=os.environ.get("WH_CLANG_TIDY_JOBS", "4"))
    clang_tidy_parser.set_defaults(func=ci_clang_tidy)

    codechecker_parser = ci_sub.add_parser("codechecker")
    codechecker_parser.add_argument("--configure-preset", default=DEFAULT_ANALYSIS_PRESET)
    codechecker_parser.add_argument(
        "--jobs",
        type=int,
        default=int(os.environ.get("WH_CODECHECKER_JOBS", "4")),
    )
    codechecker_parser.set_defaults(func=ci_codechecker)

    coverage_parser = ci_sub.add_parser("coverage")
    coverage_parser.add_argument("--configure-preset", default=DEFAULT_COVERAGE_PRESET)
    coverage_parser.add_argument("--build-preset")
    coverage_parser.add_argument("--coverage-min-lines", type=float, default=0.70)
    add_shard_args(coverage_parser)
    coverage_parser.set_defaults(func=ci_coverage)

    coverage_shard_parser = ci_sub.add_parser("coverage-shard")
    coverage_shard_parser.add_argument("--configure-preset", default=DEFAULT_COVERAGE_PRESET)
    coverage_shard_parser.add_argument("--build-preset")
    coverage_shard_parser.add_argument("--profile-output", type=Path)
    coverage_shard_parser.add_argument("--artifact-dir", type=Path)
    add_shard_args(coverage_shard_parser)
    coverage_shard_parser.set_defaults(func=ci_coverage_shard)

    coverage_aggregate_parser = ci_sub.add_parser("coverage-aggregate")
    coverage_aggregate_parser.add_argument("--artifact-dir", type=Path, required=True)
    coverage_aggregate_parser.add_argument("--coverage-min-lines", type=float, default=0.70)
    coverage_aggregate_parser.set_defaults(func=ci_coverage_aggregate)

    return parser


def main() -> int:
    os.chdir(ROOT)
    parser = build_parser()
    args = parser.parse_args()
    args.func(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
