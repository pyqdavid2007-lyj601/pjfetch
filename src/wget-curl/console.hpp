#ifndef WGET_CURL_CONSOLE_HPP
#define WGET_CURL_CONSOLE_HPP

// console 隔离层 —— 全项目唯一直接接触平台 API 的 CLI 位置（#ifdef _WIN32 双分支）：
//   Windows 自实现 CommandLineToArgvW 等价的引号规则解析 GetCommandLineW，避免 shell32 链接依赖；
//   Ctrl+C / SIGTERM 一律只置原子标志，由主循环轮询后调 pjf_download_cancel。

#include <atomic>
#include <string>
#include <vector>

namespace pjf_cli {

// Ctrl+C / SIGTERM 时由处理器置位；主循环轮询后调用 pjf_download_cancel。
extern std::atomic<bool> g_cancel_flag;

// 安装控制台处理器、（Windows）设置 UTF-8 输出码页。进程内调用一次。
void console_init();

// stderr 是否为终端（进度重绘据此开关）。
bool console_is_tty();

// 终端列宽；非终端或查询失败返回 0。
int console_width();

// 整条命令行 -> UTF-8 参数向量（不含 argv[0]）。
std::vector<std::string> console_command_line(int argc, char** argv);

} // namespace pjf_cli

#endif // WGET_CURL_CONSOLE_HPP
