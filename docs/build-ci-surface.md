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
./build.sh sync-third-party
./build.sh editor
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
./build.sh configure --preset dev-clang-release
./build.sh configure --preset dev-gcc-release
```

The root wrapper simply forwards to `scripts/toolchain.py local ...`.
`sync-third-party` is the public command name; the checked-in dependency tree
still lives under `thirdy_party/` as an internal path.

## CI Surface

GitHub Actions now call `scripts/toolchain.py ci ...` directly.

Tracked trigger workflows:

- `01-pr-fast-gates.yml`
  - thin PR/manual trigger for fast gates
- `02-build-test-matrix.yml`
  - thin matrix trigger for cross-platform `ci.pr` build/test shards
- `03-deep-quality.yml`
  - thin trigger for clang-tidy, CodeChecker, ASan/UBSan, TSan
- `04-security-coverage.yml`
  - thin trigger for dependency review, Trivy, and Linux coverage shards / aggregate
- `05-nightly-stress.yml`
  - thin trigger for Linux `ci.nightly` stress shards
- `06-manual-debug.yml`
  - manual debug lane with explicit runner, preset, shard, and label inputs

Tracked reusable workflows:

- `reusable-fast-gates.yml`
- `reusable-build-test.yml`
- `reusable-analysis.yml`
- `reusable-coverage-shard.yml`
- `reusable-coverage-aggregate.yml`

Build/test and sanitizer shards are now planned from the test manifest and a
build-action budget, rather than hard-coded target counts. Coverage now uses
the same source-layout manifest planning model, with shard-local profile
collection and a final aggregate gate. CI execution bodies are now centralized
in reusable workflows so checkout, environment setup, compiler cache, and
diagnostic artifact handling are no longer copy-pasted across each trigger
workflow. CodeQL is provided by GitHub's default Code Scanning setup instead
of a duplicate repo-local workflow job. Tracked workflows currently use a 200
build-action shard budget by default, and matrix jobs now set explicit
`max-parallel` limits to avoid flooding runners.

Representative CI commands:

```bash
python3 scripts/toolchain.py ci fast-gates
python3 scripts/toolchain.py ci emit-test-matrix --config .github/matrices/build-test.json --max-build-actions-per-shard 200 --output /tmp/build-test-matrix.json
python3 scripts/toolchain.py ci build-test --configure-preset ci-linux-debug --build-preset ci-linux-debug --shard-count <resolved-shard-count> --shard-index <resolved-shard-index> --include-label ci.pr --exclude-label ci.nightly
python3 scripts/toolchain.py ci clang-tidy --configure-preset ci-static-analysis --shard-count 4 --shard-index 0
python3 scripts/toolchain.py ci emit-test-matrix --config .github/matrices/sanitizer.json --max-build-actions-per-shard 200 --output /tmp/sanitizer-matrix.json
python3 scripts/toolchain.py ci sanitizer --configure-preset ci-asan-ubsan --build-preset ci-asan-ubsan --shard-count <resolved-shard-count> --shard-index <resolved-shard-index>
python3 scripts/toolchain.py ci emit-test-matrix --config .github/matrices/coverage.json --max-build-actions-per-shard 200 --output /tmp/coverage-matrix.json
python3 scripts/toolchain.py ci coverage-shard --configure-preset ci-coverage --build-preset ci-coverage --shard-count <resolved-shard-count> --shard-index <resolved-shard-index> --artifact-dir /tmp/coverage-artifact-0
python3 scripts/toolchain.py ci coverage-aggregate --artifact-dir /tmp/coverage-artifacts --coverage-min-lines 0.70
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
- `tools/**`

The deleted legacy layers are intentionally no longer part of the supported
surface:

- `.github/actions/run/**`
- `scripts/ci/jobs/**`
- `scripts/ci/checks/**`
- `scripts/build/**`
