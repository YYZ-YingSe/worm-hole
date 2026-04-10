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
THIRD_PARTY_DIR = ROOT / "thirdy_party"
BUILD_ROOT = ROOT / "build"
DEFAULT_REPORTER = "compact"
DEFAULT_LOCAL_PRESET = "dev-debug"
DEFAULT_ANALYSIS_PRESET = "ci-static-analysis"
DEFAULT_SANITIZER_PRESET = "ci-asan-ubsan"
DEFAULT_COVERAGE_PRESET = "ci-coverage"
DEFAULT_CODEQL_PRESET = "ci-codeql"


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


def build_dir_for_preset(preset: str) -> Path:
    return BUILD_ROOT / preset


def sync_compile_commands(build_dir: Path) -> None:
    source = build_dir / "compile_commands.json"
    target = ROOT / "compile_commands.json"
    if not source.exists():
        return
    shutil.copyfile(source, target)
    print_step("compile-commands", f"SYNC {source} -> {target}")


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
        if under(file, ROOT) and not under(file, THIRD_PARTY_DIR) and not under(file, BUILD_ROOT)
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
        if under(file, ROOT) and not under(file, THIRD_PARTY_DIR) and not under(file, BUILD_ROOT)
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
        except subprocess.TimeoutExpired:
            fail("test-shard", f"target {entry.target} timed out after {timeout_seconds}s", code=124)

        elapsed = time.monotonic() - start
        print_step("test-shard", f"DONE {entry.target} elapsed={elapsed:.2f}s")
        if completed.returncode != 0:
            raise SystemExit(completed.returncode)

    print_step("test-shard", f"PASS {len(entries)} test targets")


def filter_compile_commands_to_project_scope(compile_db: Path, target_file: Path) -> None:
    entries = json.loads(compile_db.read_text())
    filtered: list[dict[str, object]] = []
    for entry in entries:
        file_value = entry.get("file")
        if not file_value:
            continue

        file_path = Path(str(file_value))
        if not file_path.is_absolute():
            directory = Path(str(entry.get("directory", ROOT)))
            file_path = (directory / file_path).resolve()
        else:
            file_path = file_path.resolve()

        if not under(file_path, ROOT):
            continue
        if under(file_path, THIRD_PARTY_DIR):
            continue
        if under(file_path, BUILD_ROOT):
            continue
        filtered.append(entry)

    if not filtered:
        fail("compile-commands", "no project translation units after filtering")

    target_file.write_text(json.dumps(filtered, indent=2) + "\n")
    print_step("compile-commands", f"FILTER {len(filtered)}/{len(entries)} -> {target_file}")


def load_compile_entries(compile_db: Path) -> list[CompileEntry]:
    raw_entries = json.loads(compile_db.read_text())
    dedup: dict[Path, CompileEntry] = {}
    for entry in raw_entries:
        file_value = entry.get("file")
        if not file_value:
            continue

        file_path = Path(str(file_value))
        if not file_path.is_absolute():
            directory = Path(str(entry.get("directory", ROOT)))
            file_path = (directory / file_path).resolve()
        else:
            file_path = file_path.resolve()

        if not file_path.exists():
            continue
        if not under(file_path, ROOT):
            continue
        if under(file_path, THIRD_PARTY_DIR):
            continue
        if under(file_path, BUILD_ROOT):
            continue

        dedup[file_path] = CompileEntry(file=file_path, size=max(1, file_path.stat().st_size))
    return list(dedup.values())


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
    build_preset(build_preset_name, tag, targets=targets)
    print_compiler_cache_stats()
    run_test_entries(
        shard,
        reporter=reporter,
        default_timeout_seconds=default_timeout_seconds,
        env=env,
    )
    print_step(tag, "PASS")


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

    entries = load_compile_entries(compile_db)
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
    filter_compile_commands_to_project_scope(compile_db, filtered_compile_db)
    if report_dir.exists():
        shutil.rmtree(report_dir)
    if report_json.exists():
        report_json.unlink()

    print_step("codechecker", f"preset: {args.configure_preset}")
    run(
        [
            codechecker_bin,
            "analyze",
            str(filtered_compile_db),
            "--output",
            str(report_dir),
            "--analyzers",
            "clangsa",
            "--jobs",
            str(args.jobs),
            "--clean",
        ]
    )
    run([codechecker_bin, "parse", str(report_dir), "-e", "json", "-o", str(report_json)])
    payload = json.loads(report_json.read_text())
    issue_count = len(payload.get("reports", []))
    if issue_count != 0:
        subprocess.run([codechecker_bin, "parse", str(report_dir)], check=False)
        fail("codechecker", f"findings={issue_count}")
    print_step("codechecker", "PASS")


def ci_codeql_build(args: argparse.Namespace) -> None:
    if not (ROOT / "CMakeLists.txt").exists():
        print_step("codeql-build", "SKIP no CMakeLists.txt")
        return

    build_dir = configure_preset(args.configure_preset, "codeql-build")
    build_preset(
        args.build_preset or args.configure_preset,
        "codeql-build",
        targets=[build_artifact_target("tests")],
    )
    print_compiler_cache_stats()
    print_step("codeql-build", f"PASS {build_dir}")


def ci_coverage(args: argparse.Namespace) -> None:
    llvm_cov_bin = os.environ.get("WH_LLVM_COV_BIN", "llvm-cov")
    llvm_profdata_bin = os.environ.get("WH_LLVM_PROFDATA_BIN", "llvm-profdata")
    require_commands("coverage", "cmake", llvm_cov_bin, llvm_profdata_bin)

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
    build_preset(args.build_preset or args.configure_preset, "coverage", targets=targets)
    print_compiler_cache_stats()

    profile_dir = build_dir / "profiles"
    report_dir = build_dir / "reports"
    profdata_file = profile_dir / "coverage.profdata"
    report_json = report_dir / "coverage-summary.json"
    profile_dir.mkdir(parents=True, exist_ok=True)
    report_dir.mkdir(parents=True, exist_ok=True)

    for file in profile_dir.glob("*.profraw"):
        file.unlink()

    run_test_entries(
        shard,
        reporter=args.reporter,
        default_timeout_seconds=args.default_timeout_seconds,
        env={"LLVM_PROFILE_FILE": str(profile_dir / "%m-%p.profraw")},
    )

    profraw_files = sorted(profile_dir.glob("*.profraw"))
    if not profraw_files:
        fail("coverage", f"no profile data produced under {profile_dir}")

    binaries = [str(entry.executable) for entry in shard]
    run(
        [
            llvm_profdata_bin,
            "merge",
            "-sparse",
            *map(str, profraw_files),
            "-o",
            str(profdata_file),
        ]
    )

    with report_json.open("w", encoding="utf-8") as handle:
        subprocess.run(
            [
                llvm_cov_bin,
                "export",
                "--instr-profile",
                str(profdata_file),
                *binaries,
            ],
            cwd=str(ROOT),
            check=True,
            text=True,
            stdout=handle,
        )

    payload = json.loads(report_json.read_text())
    data = payload.get("data") or []
    totals = data[0].get("totals", {}) if data else {}
    lines = totals.get("lines", {}) if isinstance(totals, dict) else {}
    percent = lines.get("percent")
    if percent is None:
        fail("coverage", "coverage export did not contain line percentage")

    line_pct = float(percent)
    min_pct = float(args.coverage_min_lines) * 100.0
    if line_pct + 1e-9 < min_pct:
        fail("coverage", f"line coverage {line_pct:.2f}% below threshold {min_pct:.2f}%")
    print_step("coverage", f"PASS line coverage {line_pct:.2f}% (threshold {min_pct:.2f}%)")


def local_sync_thirdy_party(_: argparse.Namespace) -> None:
    require_commands("sync-thirdy-party", "git")
    run(["git", "submodule", "update", "--init", "--recursive"])
    print_step("sync-thirdy-party", "PASS")


def local_clean(args: argparse.Namespace) -> None:
    target = BUILD_ROOT if args.all else build_dir_for_preset(args.preset)
    if target.exists():
        shutil.rmtree(target)
        print_step("clean", f"REMOVED {target}")
    else:
        print_step("clean", f"SKIP missing {target}")


def local_configure(args: argparse.Namespace) -> None:
    build_dir = configure_preset(args.preset, "configure", cache_entries=args.define)
    print_step("configure", f"PASS {build_dir}")


def local_build(args: argparse.Namespace) -> None:
    configure_preset(args.preset, "build", cache_entries=args.define)
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

    local_sync_parser = local_sub.add_parser("sync-thirdy-party")
    local_sync_parser.set_defaults(func=local_sync_thirdy_party)

    local_clean_parser = local_sub.add_parser("clean")
    local_clean_parser.add_argument("--preset", default=DEFAULT_LOCAL_PRESET)
    local_clean_parser.add_argument("--all", action="store_true")
    local_clean_parser.set_defaults(func=local_clean)

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

    codeql_parser = ci_sub.add_parser("codeql-build")
    codeql_parser.add_argument("--configure-preset", default=DEFAULT_CODEQL_PRESET)
    codeql_parser.add_argument("--build-preset")
    codeql_parser.set_defaults(func=ci_codeql_build)

    coverage_parser = ci_sub.add_parser("coverage")
    coverage_parser.add_argument("--configure-preset", default=DEFAULT_COVERAGE_PRESET)
    coverage_parser.add_argument("--build-preset")
    coverage_parser.add_argument("--coverage-min-lines", type=float, default=0.70)
    add_shard_args(coverage_parser)
    coverage_parser.set_defaults(func=ci_coverage)

    return parser


def main() -> int:
    os.chdir(ROOT)
    parser = build_parser()
    args = parser.parse_args()
    args.func(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
