# FT Test Layer

`tests/FT` is the active functional-test layer.

Directory rules:
- Active suites live under `tests/FT/wh/...`
- The subtree mirrors public `include/wh/...` categories
- Paths should be category-first, then feature-first

Examples:
- `tests/FT/wh/core/bounded_queue/bounded_queue_contracts_test.cpp`
- `tests/FT/wh/compose/graph/stress_test.cpp`
- `tests/FT/wh/model/component_model_contracts_test.cpp`
- `tests/FT/wh/tool/component_tool_contracts_test.cpp`

This layer owns:
- cross-module contract tests
- feature-level integration and workflow tests
- graph regression and stress suites

This layer does not own:
- header-mapped unit tests in `tests/UT`
- benchmarks in `benchmark`

CTest discovery for this layer is scoped to `tests/FT/wh/**/*_test.cpp`
and every discovered case is prefixed with `FT::`.
