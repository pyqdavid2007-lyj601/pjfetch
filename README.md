# pjfetch

开源并发下载器（IDM 替代）：基于 libgo（协程）+ cpr（HTTP）。

- `libparajobs` — libgo 的 C ABI 封装（并行任务引擎）
- `libpjfetch` — 多段并发下载库（C ABI，支持断点续传）
- `wget-curl` — 命令行工具

## 快速开始

```bash
git clone --recursive https://github.com/pyqdavid2007-lyj601/pjfetch.git
cd pjfetch
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCPR_CURL_USE_LIBPSL=OFF
cmake --build build
cd build && ctest --timeout 180
```

## 用法

```bash
bin/wget-curl para-jobs=8 -O out.bin https://example.com/file.bin
bin/wget-curl -c -O out.bin https://example.com/file.bin   # 断点续传
```

`para-jobs=<N>`：并行分段数（1..32，默认 8）。全部选项见 `wget-curl --help` 或 [docs/html/wget-curl.1.html](docs/html/wget-curl.1.html)。

## 说明

- 依赖（libgo / cpr / cpputest）均为 Git 子模组；libgo 的必需小补丁在配置期自动应用（见 `patches/`）。
- 要求：CMake ≥ 3.28、Ninja、MSVC（或 GCC/Clang）、首次配置需联网。
- 测试：124 个 CppUTest 用例，`ctest` 驱动，测试服务器无窗口弹出。
- 更多细节：[docs/html/](docs/html/)（三份手册：wget-curl / libpjfetch / libparajobs）。
