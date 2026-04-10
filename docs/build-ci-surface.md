# Build and CI Surface

`worm-hole` keeps a small public build and verification surface.

## Public Entrypoints

- `./build.sh`
  - unified local entrypoint for configure, build, and test
- `scripts/ci/run_phase01_gates.sh`
  - local fast gate driver
- `scripts/nightly/nightly_cmake_test_driver.sh`
  - local nightly sanitizer and labeled-suite driver

These are the entrypoints intended for humans. Internal helper scripts remain
free to move as long as the public surface stays stable.

## Internal Layers

The following paths are implementation layers rather than stable user-facing
interfaces:

- `scripts/build/**`
- `scripts/ci/**` except `scripts/ci/run_phase01_gates.sh`
- `.github/actions/**`
- `.github/workflows/**`

## Workflow Overview

Tracked GitHub Actions workflows:

- `01-pr-fast-gates.yml`
  - pull-request fast checks
- `02-build-test-matrix.yml`
  - cross-platform build and test matrix
- `03-deep-quality.yml`
  - clang-tidy, backend static analysis, sanitizer smoke
- `04-security-coverage.yml`
  - codeql, dependency review, SCA, coverage
- `05-nightly-stress.yml`
  - nightly sanitizer and stress-oriented jobs

## Common Local Commands

Debug build:

```bash
./build.sh --configure --build-type Debug
./build.sh --build --build-type Debug
```

Release build with tests:

```bash
./build.sh --configure --build-type Release --enable-tests --disable-examples --disable-benchmarks
./build.sh --build --build-type Release
```

List tests:

```bash
./build.sh --test --list-tests
```

Run only unit tests:

```bash
./build.sh --test --build-type Debug --test-scope ut
```

Run only functional tests:

```bash
./build.sh --test --build-type Debug --test-scope ft
```

Nightly local driver examples:

```bash
bash scripts/nightly/nightly_cmake_test_driver.sh sanitizer asan
bash scripts/nightly/nightly_cmake_test_driver.sh label stress
```
