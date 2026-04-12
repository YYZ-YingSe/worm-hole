# Contributing to worm-hole

Thanks for contributing.

`worm-hole` is a C++20 header-first project. Most public surface changes happen
under `include/wh`, so good contributions keep public behavior, tests, docs,
and examples aligned.

## Start Here

- Support and troubleshooting: [`docs/README.md`](docs/README.md) and
  [`docs/troubleshooting.md`](docs/troubleshooting.md)
- Community expectations: [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md)
- Security reporting: [`SECURITY.md`](SECURITY.md)

## Local Setup

Sync submodules before the first local build:

```bash
./build.sh --sync-thirdy-party
```

Configure, build, and run tests with the repository entrypoint:

```bash
./build.sh --all --build-type Debug --enable-tests
```

Useful variants:

```bash
./build.sh --build --build-type Release --enable-tests
./build.sh --test --build-type Debug --test-scope ut
./build.sh --test --build-type Debug --test-scope ft
./build.sh --test --build-type Debug --ctest-filter <regex>
./build.sh --reconfigure --build-type Debug --enable-tests
```

If you are debugging locally and current compiler warnings are not yet ready to
be fixed, you can temporarily disable warnings-as-errors:

```bash
./build.sh --build --build-type Debug --no-werror
```

## Code Style

- Format changed C++ files with the repository `.clang-format`.
- Keep public headers under `include/wh` self-contained.
- Prefer small, reviewable patches over unrelated mixed refactors.
- Update docs and examples when public-facing behavior changes.

## Tests

Use the existing repository split:

- `tests/UT`: header-mapped unit tests
- `tests/FT`: feature-level and cross-module functional tests

Repository conventions:

- `tests/UT` mirrors `include/wh`
- `tests/FT` is category-first and feature-first under `tests/FT/wh/...`
- public API changes should come with meaningful verification that proves the
  behavior and guards regressions

At minimum, run the tests that cover the changed surface. For public-header
changes that can affect cross-module behavior, run both UT and FT.

## Pull Requests

Please include:

- what changed
- why it changed
- how you verified it
- any known follow-up or limitations

The repository also includes a pull request template at
`.github/PULL_REQUEST_TEMPLATE.md`.
