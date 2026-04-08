# UT Layout

`tests/UT` mirrors `include/wh`.

Rules:

- Every header under `include/wh` maps to exactly one primary UT file.
- The UT path mirrors the header path and swaps `.hpp` for `_ut.cpp`.
- `TEST_CASE` tags use `[UT][path][symbol]`.
- `path` is the header path relative to `include/`.
- `symbol` is the function or method name by default.
- Extra tags such as `[branch]`, `[boundary]`, and `[concurrency]` may follow.
- Shared probes, schedulers, mocks, and sender helpers stay in `tests/helper`.
- Real problems found while writing UT must not be bypassed by special-case samples;
  they must be preserved, tracked, and included in the final unified report.
- `scripts/testing/verify_ut_manifest.py --summary-json <path>` emits a
  machine-readable coverage snapshot for the final unified report.
- `scripts/testing/audit_ut_quality.py --summary-json <path>` audits `[UT][path][symbol]`
  path consistency plus file-level `[condition]`, `[branch]`, `[boundary]`, and
  concurrency-candidate tag coverage.

Examples:

- `include/wh/core/error.hpp` -> `tests/UT/wh/core/error_ut.cpp`
- `include/wh/core/stdexec/detail/scheduled_drive_loop.hpp` ->
  `tests/UT/wh/core/stdexec/detail/scheduled_drive_loop_ut.cpp`
- `include/wh/flow.hpp` -> `tests/UT/wh/flow_ut.cpp`
