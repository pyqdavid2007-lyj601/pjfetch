#include "console.hpp"

#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>     // _isatty, _fileno
#else
#include <csignal>
#include <unistd.h>
#include <sys/ioctl.h>
#endif

namespace pjf_cli {

std::atomic<bool> g_cancel_flag{false};

#ifdef _WIN32

static BOOL WINAPI ctrl_handler(DWORD type) {
    (void)type;  // CTRL_C_EVENT / CTRL_BREAK_EVENT / 关闭事件一律同样处理
    g_cancel_flag.store(true);
    return TRUE;  // 立即返回，进程交还给主循环正常收尾
}

void console_init() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
}

bool console_is_tty() {
    return _isatty(_fileno(stderr)) != 0;
}

int console_width() {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &csbi)) {
        int w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        return w > 0 ? w : 0;
    }
    return 0;
}

// GetCommandLineW 的引号规则（等价 CommandLineToArgvW，免 shell32 链接依赖）：空白分界；
// " 切换引用态；2n 个 \ + " -> n 个 \ + 字面 "（不切换）；n 个 \ + "（n 偶）-> n 个 \ 并切换。
static std::vector<std::wstring> parse_wcmdline(const wchar_t* cmd) {
    std::vector<std::wstring> out;
    const wchar_t* p = cmd;
    while (*p) {
        while (*p == L' ' || *p == L'\t') ++p;
        if (!*p) break;
        std::wstring arg;
        bool in_quotes = false;
        int backslashes = 0;
        for (; *p; ++p) {
            if (*p == L'\\') { ++backslashes; continue; }
            if (*p == L'"') {
                arg.append(size_t(backslashes / 2), L'\\');
                if (backslashes % 2) {
                    arg.push_back(L'"');           // \" -> 字面引号，不切换
                } else {
                    in_quotes = !in_quotes;        // 引号切换引用态
                }
                backslashes = 0;
            } else {
                arg.append(size_t(backslashes), L'\\');
                backslashes = 0;
                if (!in_quotes && (*p == L' ' || *p == L'\t')) break;
                arg.push_back(*p);
            }
        }
        arg.append(size_t(backslashes), L'\\');
        out.push_back(std::move(arg));
    }
    return out;
}

static std::string wide_to_utf8(const wchar_t* s, int len) {
    if (len <= 0) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s, len, &out[0], n, nullptr, nullptr);
    return out;
}

std::vector<std::string> console_command_line(int /*argc*/, char** /*argv*/) {
    std::vector<std::string> out;
    std::vector<std::wstring> parts = parse_wcmdline(GetCommandLineW());
    if (!parts.empty()) parts.erase(parts.begin());  // 丢弃 argv[0]
    out.reserve(parts.size());
    for (const std::wstring& w : parts) out.push_back(wide_to_utf8(w.c_str(), int(w.size())));
    return out;
}

#else // POSIX

void console_init() {
    std::signal(SIGINT, [](int) { g_cancel_flag.store(true); });
    std::signal(SIGTERM, [](int) { g_cancel_flag.store(true); });
}

bool console_is_tty() {
    return isatty(2) == 1;
}

int console_width() {
    struct winsize ws;
    if (ioctl(2, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return int(ws.ws_col);
    return 0;
}

std::vector<std::string> console_command_line(int argc, char** argv) {
    std::vector<std::string> out;
    for (int i = 1; i < argc; ++i)
        if (argv[i]) out.push_back(argv[i]);
    return out;
}

#endif // _WIN32

} // namespace pjf_cli
