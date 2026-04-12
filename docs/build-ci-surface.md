# Build and CI Surface

`worm-hole` now uses one toolchain orchestrator for local workflows and GitHub
Actions.

## Public Entrypoints

- `./build.sh`
  - human-friendly wrapper for local configure, build, test, clean, and
    submodule sync
- `scripts/toolchain.py`
  - canonical machine-oriented entrypoint for local automation and CI
- `CMakePresets.json`
  - shared configure/build presets for local and CI execution
- `CTestPresets.json`
  - shared CTest preset registry

## Local Surface

Typical local flow:

```bash
./build.sh sync-thirdy-party
./build.sh configure --preset dev-debug
./build.sh build --preset dev-debug --artifacts tests
./build.sh test --preset dev-debug --build-first
```

Common variants:

```bash
./build.sh clean --preset dev-debug
./build.sh test --preset dev-debug --build-first --suite UT
./build.sh test --preset dev-debug --build-first --suite FT
./build.sh build --preset dev-debug --define WH_BUILD_EXAMPLES=ON
./build.sh build --preset dev-release --define WH_BUILD_BENCHMARKS=ON
```

The root wrapper simply forwards to `scripts/toolchain.py local ...`.

## CI Surface

GitHub Actions now call `scripts/toolchain.py ci ...` directly.

Tracked workflows:

- `01-pr-fast-gates.yml`
  - actionlint, gitleaks, shellcheck, clang-format
- `02-build-test-matrix.yml`
  - cross-platform build/test shards for `ci.pr`
- `03-deep-quality.yml`
  - clang-tidy, CodeChecker, ASan/UBSan, TSan
- `04-security-coverage.yml`
  - CodeQL, dependency review, Trivy, LLVM coverage
- `05-nightly-stress.yml`
  - nightly `ci.nightly` heavy-test shards

Build/test and sanitizer shards are now planned from the test manifest and a
build-action budget, rather than hard-coded target counts. Coverage uses the
dedicated `ci-coverage` preset and the `coverage-monolith` test executable
layout.

Representative CI commands:

```bash
python3 scripts/toolchain.py ci fast-gates
python3 scripts/toolchain.py ci emit-test-matrix --config .github/matrices/build-test.json --max-build-actions-per-shard 200 --output /tmp/build-test-matrix.json
python3 scripts/toolchain.py ci build-test --configure-preset ci-linux-debug --build-preset ci-linux-debug --shard-count <resolved-shard-count> --shard-index <resolved-shard-index> --include-label ci.pr --exclude-label ci.nightly
python3 scripts/toolchain.py ci clang-tidy --configure-preset ci-static-analysis --shard-count 4 --shard-index 0
python3 scripts/toolchain.py ci emit-test-matrix --config .github/matrices/sanitizer.json --max-build-actions-per-shard 200 --output /tmp/sanitizer-matrix.json
python3 scripts/toolchain.py ci sanitizer --configure-preset ci-asan-ubsan --build-preset ci-asan-ubsan --shard-count <resolved-shard-count> --shard-index <resolved-shard-index>
python3 scripts/toolchain.py ci coverage --configure-preset ci-coverage --coverage-min-lines 0.70
```

`<resolved-shard-count>` and `<resolved-shard-index>` come from the planner
output and are intentionally not fixed constants.

## Internal Implementation Layers

The following paths are implementation layers rather than user-facing
contracts:

- `.github/workflows/**`
- `.github/actions/prepare/**`
- `.github/actions/setup/**`
- `scripts/ci/setup/**`
- `cmake/**`

The deleted legacy layers are intentionally no longer part of the supported
surface:

- `.github/actions/run/**`
- `scripts/ci/jobs/**`
- `scripts/ci/checks/**`
- `scripts/build/**`
