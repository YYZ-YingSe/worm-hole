# Troubleshooting

## Fresh checkout does not build

`worm-hole` uses in-tree third-party dependencies. If the repository was cloned
without submodules, synchronize them first:

```bash
./build.sh --sync-thirdy-party
```

## Build directory looks stale

If CMake cache or generated artifacts no longer match the current branch, start
from a clean build root:

```bash
./build.sh --clean-root --configure --build --build-type Debug --enable-tests
```

For a lighter reset of only the selected build directory:

```bash
./build.sh --clean --configure --build --build-type Debug --enable-tests
```

## A local compiler trips on warnings-as-errors

The repository normally builds with warnings-as-errors enabled. During local
investigation you can disable that gate temporarily:

```bash
./build.sh --build --build-type Debug --no-werror
```

This should not be treated as a substitute for fixing real warnings before a
final contribution.

## I only want one test layer

Use test scopes instead of running everything:

```bash
./build.sh --test --build-type Debug --test-scope ut
./build.sh --test --build-type Debug --test-scope ft
```

You can also narrow to a CTest regex:

```bash
./build.sh --test --build-type Debug --ctest-filter <regex>
```

## I need examples or benchmarks

Examples and benchmarks are opt-in:

```bash
./build.sh --build --build-type Debug --enable-examples
./build.sh --build --build-type Release --enable-benchmarks
```

## Platform expectations

The CI matrix currently exercises:

- Ubuntu
- macOS
- Windows
- Debug and Release builds

Nightly jobs additionally cover sanitizer and stress-oriented workloads on
Linux.

## More test-layout details

- Unit-test layout: [`tests/UT/README.md`](../tests/UT/README.md)
- Functional-test layout: [`tests/FT/README.md`](../tests/FT/README.md)
