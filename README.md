# worm-hole

[简体中文](README.zh-CN.md)

[![Build and Test](https://github.com/YYZ-YingSe/worm-hole/actions/workflows/02-build-test-matrix.yml/badge.svg?branch=main)](https://github.com/YYZ-YingSe/worm-hole/actions/workflows/02-build-test-matrix.yml)
[![Deep Quality](https://github.com/YYZ-YingSe/worm-hole/actions/workflows/03-deep-quality.yml/badge.svg?branch=main)](https://github.com/YYZ-YingSe/worm-hole/actions/workflows/03-deep-quality.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

`worm-hole` is a C++20 toolkit for building typed agent systems, compiled
workflow graphs, retrieval/indexing flows, and shared schema/runtime layers
under one execution model.

It is designed for projects that want prompts, models, tools, flows, and
authored agents to lower into explicit graphs instead of living in separate
runtime silos.

## Overview

`worm-hole` brings together:

- authored agent shells such as ReAct
- compose graph lowering and execution
- typed component contracts for prompt, model, tool, retriever, indexer, and
  document boundaries
- reusable retrieval and indexing flows
- shared runtime and stream infrastructure for C++20 applications

## Why worm-hole

- One execution model across prompts, models, tools, retrieval, indexing, and
  agents.
- Public contracts live under `include/wh` instead of being hidden behind
  ad-hoc glue.
- Feature flows remain testable through normal C++ build and CI workflows.
- The repository treats public headers, UT, FT, examples, and CI as one
  coherent project surface.

## When to Use It

Use `worm-hole` when you want:

- a C++20-native agent or workflow toolkit
- explicit graph lowering and typed integration boundaries
- one runtime model for tools, retrieval, indexing, and authored agents
- low-level or embedded integration without adopting a hosted AI platform

## When Not to Use It

`worm-hole` is probably the wrong fit when you want:

- a turnkey hosted AI product
- a Python-first rapid experimentation stack
- an out-of-the-box UI, persistence, and deployment platform

## Architecture at a Glance

```text
Prompt Template
      |
      v
Rendered Messages
      |
      v
Authored Agent Shell
      |
      v
Lowered Compose Graph
      |
      +--> Chat Model
      +--> Tool Node ---> Retrieval / Indexing Flows ---> Typed Components
      +--> Shared Runtime / Stream / Schema Layers
```

## Representative Example

The repository includes a full end-to-end example here:

- [`example/react_kb_agent.cpp`](example/react_kb_agent.cpp)

That example:

- indexes a small handbook with `wh::flow::indexing::parent`
- exposes retrieval through `wh::flow::retrieval::multi_query`
- wraps retrieval as a `search_kb` tool
- renders the initial conversation with `wh::prompt::simple_chat_template`
- runs a ReAct agent whose model emits a tool call and then answers from the
  tool result

Flow wrappers follow the same authored-surface contract as the rest of the
project: configure first, call `freeze()`, then execute or wire them into a
graph node. Agent shells follow the matching public phase path:
`freeze() -> into_agent() -> lower()` before invocation.

Example build entrypoint:

```bash
./build.sh build --preset dev-debug --define WH_BUILD_EXAMPLES=ON
```

## CMake Integration

The public CMake surface is the `wh::core` target.

```cmake
add_subdirectory(path/to/worm-hole)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE wh::core)
```

## Build and Test

The repository now exposes one preset-driven local surface:

```bash
./build.sh configure --preset dev-debug
./build.sh build --preset dev-debug --artifacts tests
./build.sh test --preset dev-debug --build-first
```

Common variants:

```bash
./build.sh test --preset dev-debug --build-first --suite UT
./build.sh test --preset dev-debug --build-first --suite FT
./build.sh build --preset dev-release --define WH_BUILD_BENCHMARKS=ON
./build.sh build --preset dev-debug --define WH_BUILD_EXAMPLES=ON
```

For machine-oriented automation, the canonical orchestrator is:

```bash
python3 scripts/toolchain.py --help
```

## Supported Surface

- CMake 3.25+
- C++20
- build/test CI across Ubuntu, macOS, and Windows
- deep analysis, coverage, and nightly stress shards on Linux

## Documentation

- Docs index: [`docs/README.md`](docs/README.md)
- Troubleshooting: [`docs/troubleshooting.md`](docs/troubleshooting.md)
- Build and CI surface: [`docs/build-ci-surface.md`](docs/build-ci-surface.md)
- UT layout: [`tests/UT/README.md`](tests/UT/README.md)
- FT layout: [`tests/FT/README.md`](tests/FT/README.md)

## Community

- Support: see [`docs/README.md`](docs/README.md) and
  [`docs/troubleshooting.md`](docs/troubleshooting.md)
- Contributing: [`CONTRIBUTING.md`](CONTRIBUTING.md)
- Code of conduct: [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md)
- Security: [`SECURITY.md`](SECURITY.md)

## Project Status

`worm-hole` is under active development. Public interfaces live under
`include/wh`, but agent, graph, and stream surfaces are still being tightened
and refined.

## License

This project is released under the Apache License 2.0.

- [`LICENSE`](LICENSE)
