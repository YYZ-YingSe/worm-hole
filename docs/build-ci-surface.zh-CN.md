# 构建与 CI 面

`worm-hole` 只保留少量公开的构建与验证入口。

## 对人公开的入口

- `./build.sh`
  - 本地 configure / build / test 的统一入口
- `scripts/ci/run_phase01_gates.sh`
  - 本地快速门禁入口
- `scripts/nightly/nightly_cmake_test_driver.sh`
  - 本地 nightly sanitizer / label suite 驱动入口

这些入口是给人直接使用的。  
只要这几个入口保持稳定，内部脚本路径可以继续调整。

## 内部实现层

下面这些路径属于实现层，不承诺是稳定人工接口：

- `scripts/build/**`
- `scripts/ci/**`，但不含 `scripts/ci/run_phase01_gates.sh`
- `.github/actions/**`
- `.github/workflows/**`

## Workflow 概览

当前跟踪的 GitHub Actions workflow：

- `01-pr-fast-gates.yml`
  - PR 快速门禁
- `02-build-test-matrix.yml`
  - 跨平台 build/test 矩阵
- `03-deep-quality.yml`
  - clang-tidy、后端静态分析、sanitizer smoke
- `04-security-coverage.yml`
  - codeql、dependency review、SCA、coverage
- `05-nightly-stress.yml`
  - nightly sanitizer 和压力类任务

## 常用本地命令

Debug 构建：

```bash
./build.sh --configure --build-type Debug
./build.sh --build --build-type Debug
```

带测试的 Release 构建：

```bash
./build.sh --configure --build-type Release --enable-tests --disable-examples --disable-benchmarks
./build.sh --build --build-type Release
```

列出测试：

```bash
./build.sh --test --list-tests
```

只跑单元测试：

```bash
./build.sh --test --build-type Debug --test-scope ut
```

只跑功能测试：

```bash
./build.sh --test --build-type Debug --test-scope ft
```

Nightly 本地驱动示例：

```bash
bash scripts/nightly/nightly_cmake_test_driver.sh sanitizer asan
bash scripts/nightly/nightly_cmake_test_driver.sh label stress
```
