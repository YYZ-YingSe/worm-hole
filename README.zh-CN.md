# worm-hole

[English](README.md)

[![Build and Test](https://github.com/YYZ-YingSe/worm-hole/actions/workflows/02-build-test-matrix.yml/badge.svg?branch=main)](https://github.com/YYZ-YingSe/worm-hole/actions/workflows/02-build-test-matrix.yml)
[![Deep Quality](https://github.com/YYZ-YingSe/worm-hole/actions/workflows/03-deep-quality.yml/badge.svg?branch=main)](https://github.com/YYZ-YingSe/worm-hole/actions/workflows/03-deep-quality.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

`worm-hole` 是一个面向 C++20 的工具库，用来在同一套执行模型下构建类型化
agent、可编译 workflow/graph、retrieval/indexing flow，以及共享的
schema/runtime 层。

它适合那些希望把 prompt、model、tool、flow 和 authored agent 都 lower 到
显式图里的项目，而不是把这些能力拆散在几套互相隔离的 runtime 里。

## 项目概览

`worm-hole` 聚合了这些能力：

- ReAct 等 authored agent shell
- compose graph lowering 与执行
- prompt、model、tool、retriever、indexer、document 等类型化契约
- 可复用的 retrieval / indexing flow
- 面向 C++20 应用的共享 runtime 与 stream 基础设施

## 为什么是 worm-hole

- prompt、model、tool、retrieval、indexing、agent 走同一套执行模型。
- 公共契约集中在 `include/wh`，不是藏在一堆临时 glue code 后面。
- 功能流仍然可以按正常 C++ 工程方式做构建、测试和 CI。
- 公共头文件、UT、FT、example、CI 被当成一个整体工程面来维护。

## 什么时候适合用

当你需要下面这些能力时，`worm-hole` 比较合适：

- C++20 原生的 agent / workflow 工具库
- 显式 graph lowering 和类型化集成边界
- tool、retrieval、indexing、agent 走统一运行时模型
- 不想依赖托管 AI 平台，而是需要低层或嵌入式接入

## 什么时候不适合用

下面这些目标，`worm-hole` 通常不是最佳选择：

- 一站式托管 AI 产品
- Python-first 的快速试验框架
- 自带 UI、持久化和部署平台的开箱即用产品

## 架构一眼看懂

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

## 代表性示例

仓库里有一个完整的端到端示例：

- [`example/react_kb_agent.cpp`](example/react_kb_agent.cpp)

这条示例主线一次性覆盖了：

- 用 `wh::flow::indexing::parent` 索引一份小型知识手册
- 用 `wh::flow::retrieval::multi_query` 暴露检索能力
- 把检索包装成 `search_kb` 工具
- 用 `wh::prompt::simple_chat_template` 渲染初始对话
- 运行一个 ReAct agent，让 model 先发起 tool call，再根据工具结果回答

示例构建入口：

```bash
./build.sh build --preset dev-debug --define WH_BUILD_EXAMPLES=ON
```

## CMake 集成

公开 CMake 入口是 `wh::core`：

```cmake
add_subdirectory(path/to/worm-hole)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE wh::core)
```

## 构建与测试

当前仓库对本地只公开一套基于 preset 的入口：

```bash
./build.sh configure --preset dev-debug
./build.sh build --preset dev-debug --artifacts tests
./build.sh test --preset dev-debug --build-first
```

常用变体：

```bash
./build.sh test --preset dev-debug --build-first --suite UT
./build.sh test --preset dev-debug --build-first --suite FT
./build.sh build --preset dev-release --define WH_BUILD_BENCHMARKS=ON
./build.sh build --preset dev-debug --define WH_BUILD_EXAMPLES=ON
```

如果是自动化或 CI 场景，统一编排入口是：

```bash
python3 scripts/toolchain.py --help
```

## 支持范围

- CMake 3.25+
- C++20
- build/test CI 覆盖 Ubuntu、macOS、Windows
- 深度分析、coverage 和 nightly 重型任务运行在 Linux

## 文档入口

- 文档索引：[`docs/README.zh-CN.md`](docs/README.zh-CN.md)
- 常见问题排查：[`docs/troubleshooting.zh-CN.md`](docs/troubleshooting.zh-CN.md)
- 构建与 CI 面：[`docs/build-ci-surface.zh-CN.md`](docs/build-ci-surface.zh-CN.md)
- 单元测试布局：[`tests/UT/README.md`](tests/UT/README.md)
- 功能测试布局：[`tests/FT/README.md`](tests/FT/README.md)

## 社区与协作

- 支持入口：[`docs/README.zh-CN.md`](docs/README.zh-CN.md)、
  [`docs/troubleshooting.zh-CN.md`](docs/troubleshooting.zh-CN.md)
- 贡献说明：[`CONTRIBUTING.md`](CONTRIBUTING.md)
- 行为准则：[`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md)
- 安全说明：[`SECURITY.md`](SECURITY.md)

## 项目状态

`worm-hole` 仍处于活跃演进阶段。公共接口集中在 `include/wh`，但 agent、
graph、stream 等能力还在持续收紧和打磨。

## 许可证

本项目采用 Apache License 2.0：

- [`LICENSE`](LICENSE)
