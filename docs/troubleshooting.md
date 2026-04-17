# Troubleshooting

## Fresh checkout does not build

`worm-hole` keeps third-party dependencies in-tree. Start by syncing submodules:

```bash
./build.sh sync-third-party
```

## I want a clean local state

Remove one preset build tree:

```bash
./build.sh clean --preset dev-debug
```

Remove the whole generated build root:

```bash
python3 scripts/toolchain.py local clean --all
```

## I need a one-shot build and test run

Use the test entrypoint with `--build-first`:

```bash
./build.sh test --preset dev-debug --build-first
```

Limit the run to one suite:

```bash
./build.sh test --preset dev-debug --build-first --suite UT
./build.sh test --preset dev-debug --build-first --suite FT
```

## I need examples or benchmarks

Those are now explicit CMake defines passed through the toolchain entrypoint:

```bash
./build.sh build --preset dev-debug --define WH_BUILD_EXAMPLES=ON
./build.sh build --preset dev-release --define WH_BUILD_BENCHMARKS=ON
```

## A local compiler is blocked by warnings-as-errors

For local investigation only, reconfigure with warnings-as-errors disabled:

```bash
./build.sh configure --preset dev-debug --define WH_WARNINGS_AS_ERRORS=OFF
./build.sh build --preset dev-debug
```

Do not treat this as a substitute for fixing the underlying warning before
submitting changes.

## Which platforms does CI exercise

The tracked CI surface currently covers:

- Ubuntu
- macOS
- Windows
- debug and release build shards where they materially add signal
- Linux sanitizer, coverage, and nightly stress jobs

Repository CodeQL scanning is handled by GitHub's default Code Scanning setup
rather than a repo-local workflow.

## Where to look next

- Build and CI surface: [`build-ci-surface.md`](build-ci-surface.md)
- Unit-test layout: [`../tests/UT/README.md`](../tests/UT/README.md)
- Functional-test layout: [`../tests/FT/README.md`](../tests/FT/README.md)
