# 常见问题排查

## 新拉下来的仓库构建不起来

`worm-hole` 的第三方依赖在仓库内维护，先同步子模块：

```bash
./build.sh sync-third-party
```

## 我想把本地状态清干净

只清掉某个 preset 的构建目录：

```bash
./build.sh clean --preset dev-debug
```

把整个生成的 build 根目录都清掉：

```bash
python3 scripts/toolchain.py local clean --all
```

## 我想一条命令完成构建和测试

直接走 test 入口并开启 `--build-first`：

```bash
./build.sh test --preset dev-debug --build-first
```

如果只想跑一层：

```bash
./build.sh test --preset dev-debug --build-first --suite UT
./build.sh test --preset dev-debug --build-first --suite FT
```

## 我需要 examples 或 benchmarks

现在通过显式的 CMake define 打开：

```bash
./build.sh build --preset dev-debug --define WH_BUILD_EXAMPLES=ON
./build.sh build --preset dev-release --define WH_BUILD_BENCHMARKS=ON
```

## 编辑器或 clangd 一打开就提示头文件未找到

先配置一次专用的全量编辑器 preset：

```bash
./build.sh editor
```

这个 preset 会同时打开 tests、examples 和 benchmarks，这样
`compile_commands.json` 就能覆盖 `catch2/catch_test_macros.hpp`、
`benchmark/benchmark.h` 这类最容易缺失的头文件。

## 本地编译器被 warnings-as-errors 卡住了

如果只是本地排查，可以先关闭这条门禁再构建：

```bash
./build.sh configure --preset dev-debug --define WH_WARNINGS_AS_ERRORS=OFF
./build.sh build --preset dev-debug
```

这只能作为临时排查手段，不能代替真正修掉告警。

## 当前 CI 覆盖哪些平台

当前跟踪的 CI 面覆盖：

- Ubuntu
- macOS
- Windows
- 有实际增益的 debug / release 分片
- Linux 上的 sanitizer、coverage 和 nightly stress 任务

仓库的 CodeQL 扫描由 GitHub 默认的 Code Scanning setup 负责，不再走仓库内
单独维护的一条 workflow。

## 进一步查看哪里

- 构建与 CI 面：[`build-ci-surface.zh-CN.md`](build-ci-surface.zh-CN.md)
- 单元测试布局：[`../tests/UT/README.md`](../tests/UT/README.md)
- 功能测试布局：[`../tests/FT/README.md`](../tests/FT/README.md)
