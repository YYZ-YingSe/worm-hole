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
./build.sh sync-thirdy-party
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
```

根目录 `build.sh` 只是对 `scripts/toolchain.py local ...` 的轻包装。

## CI 使用面

GitHub Actions 现在直接调用 `scripts/toolchain.py ci ...`。

当前跟踪的 workflow：

- `01-pr-fast-gates.yml`
  - actionlint、gitleaks、shellcheck、clang-format
- `02-build-test-matrix.yml`
  - 面向 `ci.pr` 的跨平台 build/test 分片
- `03-deep-quality.yml`
  - clang-tidy、CodeChecker、ASan/UBSan、TSan
- `04-security-coverage.yml`
  - CodeQL、dependency review、Trivy、LLVM coverage
- `05-nightly-stress.yml`
  - `ci.nightly` 重型测试的 nightly 分片

代表性 CI 命令：

```bash
python3 scripts/toolchain.py ci fast-gates
python3 scripts/toolchain.py ci build-test --configure-preset ci-linux-debug --shard-count 4 --shard-index 0 --include-label ci.pr --exclude-label ci.nightly
python3 scripts/toolchain.py ci clang-tidy --configure-preset ci-static-analysis --shard-count 4 --shard-index 0
python3 scripts/toolchain.py ci sanitizer --configure-preset ci-asan-ubsan --shard-count 2 --shard-index 0
python3 scripts/toolchain.py ci coverage --configure-preset ci-coverage --coverage-min-lines 0.70
```

## 内部实现层

下面这些路径属于实现层，不是对外承诺的人类接口：

- `.github/workflows/**`
- `.github/actions/prepare/**`
- `.github/actions/setup/**`
- `scripts/ci/setup/**`
- `cmake/**`

下面这些旧层已经被移除，不再属于支持面：

- `.github/actions/run/**`
- `scripts/ci/jobs/**`
- `scripts/ci/checks/**`
- `scripts/build/**`
