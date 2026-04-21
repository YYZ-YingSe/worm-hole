# 构建与 CI 面

`worm-hole` 现在把本地流程和 GitHub Actions 都收口到同一个工具链编排入口。

## 公开入口

- `./build.sh`
  - 面向人的本地入口，负责 configure、build、test、clean 和子模块同步
- `scripts/toolchain.py`
  - 面向自动化和 CI 的标准入口
- `CMakePresets.json`
  - 本地与 CI 共用的 configure / build preset
- `CTestPresets.json`
  - 共用的 CTest preset 注册表

## 本地使用面

典型本地流程：

```bash
./build.sh sync-third-party
./build.sh editor
./build.sh configure --preset dev-debug
./build.sh build --preset dev-debug --artifacts tests
./build.sh test --preset dev-debug --build-first
```

常用变体：

```bash
./build.sh clean --preset dev-debug
./build.sh test --preset dev-debug --build-first --suite UT
./build.sh test --preset dev-debug --build-first --suite FT
./build.sh build --preset dev-debug --define WH_BUILD_EXAMPLES=ON
./build.sh build --preset dev-release --define WH_BUILD_BENCHMARKS=ON
./build.sh configure --preset dev-clang-release
./build.sh configure --preset dev-gcc-release
```

根目录 `build.sh` 只是对 `scripts/toolchain.py local ...` 的轻包装。
`sync-third-party` 是公开命令名；仓库里的依赖树路径仍然保留为内部实现用的
`thirdy_party/`。

## CI 使用面

GitHub Actions 现在直接调用 `scripts/toolchain.py ci ...`。

当前跟踪的触发 workflow：

- `01-pr-fast-gates.yml`
  - PR / 手动触发的 fast gates 薄入口
- `02-build-test-matrix.yml`
  - 面向 `ci.pr` 的跨平台 build/test 分片薄入口
- `03-deep-quality.yml`
  - clang-tidy、CodeChecker、ASan/UBSan、TSan 的薄入口
- `04-security-coverage.yml`
  - dependency review、Trivy，以及 Linux coverage 分片 / 聚合薄入口
- `05-nightly-stress.yml`
  - Linux 上 `ci.nightly` 的 stress 分片薄入口
- `06-manual-debug.yml`
  - 可手动指定 runner、preset、分片和 label 的 debug lane

当前跟踪的 reusable workflow：

- `reusable-fast-gates.yml`
- `reusable-build-test.yml`
- `reusable-analysis.yml`
- `reusable-coverage-shard.yml`
- `reusable-coverage-aggregate.yml`

现在 build/test 和 sanitizer 的分片不是写死的目标数，而是基于测试
manifest 和构建动作预算动态规划。coverage 现在也回到统一的 source
layout manifest 规划模型，分片产出 profile，最后再做一次聚合 gate。CI
执行体现在集中收敛到 reusable workflow，checkout、环境准备、编译缓存和
失败工件上传不再在每条 trigger workflow 里重复一遍。CodeQL 则改为仅使用
GitHub 默认的 Code Scanning setup，不再并行保留一份仓库内重复工作流。
当前跟踪的 workflow 默认使用每片 200 个 build actions 的预算，同时矩阵
任务显式设置了 `max-parallel`，避免一次性把 runner 打满。

代表性 CI 命令：

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

这里的 `<resolved-shard-count>` 和 `<resolved-shard-index>` 都来自矩阵规划
结果，不再是固定常量。

## 内部实现层

下面这些路径属于实现层，不是对外承诺的人类接口：

- `.github/workflows/**`
- `.github/actions/prepare/**`
- `.github/actions/setup/**`
- `scripts/ci/setup/**`
- `cmake/**`
- `tools/**`

下面这些旧层已经被移除，不再属于支持面：

- `.github/actions/run/**`
- `scripts/ci/jobs/**`
- `scripts/ci/checks/**`
- `scripts/build/**`
