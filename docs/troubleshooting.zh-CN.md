# 常见问题排查

## 新拉下来的仓库构建不起来

`worm-hole` 依赖仓库内第三方目录。如果仓库不是带子模块一起拉下来的，先同步：

```bash
./build.sh --sync-thirdy-party
```

## build 目录已经脏了

如果当前分支和现有 CMake cache / 生成物已经不一致，建议直接从干净的 build 根目录
重新开始：

```bash
./build.sh --clean-root --configure --build --build-type Debug --enable-tests
```

如果只是想清掉当前选中的构建目录，可以用：

```bash
./build.sh --clean --configure --build --build-type Debug --enable-tests
```

## 本地编译器因为 -Werror 卡住了

仓库默认按 warnings-as-errors 构建。你本地排查问题时，可以暂时关闭这条门禁：

```bash
./build.sh --build --build-type Debug --no-werror
```

这只是本地调试手段，不应该代替最终修复真实告警。

## 我只想跑某一层测试

可以直接用测试范围选项：

```bash
./build.sh --test --build-type Debug --test-scope ut
./build.sh --test --build-type Debug --test-scope ft
```

也可以再配合 CTest 正则进一步缩小范围：

```bash
./build.sh --test --build-type Debug --ctest-filter <regex>
```

## 我想构建 example 或 benchmark

example 和 benchmark 默认是按需开启的：

```bash
./build.sh --build --build-type Debug --enable-examples
./build.sh --build --build-type Release --enable-benchmarks
```

## 平台覆盖范围

当前 CI 矩阵会覆盖：

- Ubuntu
- macOS
- Windows
- Debug / Release

Nightly 额外覆盖 Linux 上的 sanitizer 与压力类任务。

## 进一步了解测试布局

- 单元测试布局：[`tests/UT/README.md`](../tests/UT/README.md)
- 功能测试布局：[`tests/FT/README.md`](../tests/FT/README.md)
