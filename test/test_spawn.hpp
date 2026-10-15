#ifndef PJFETCH_TEST_SPAWN_HPP
#define PJFETCH_TEST_SPAWN_HPP

// 测试用后台进程管理（仅测试基建）。硬约束：子进程一律以 CREATE_NO_WINDOW |
//   DETACHED_PROCESS 启动（父进程无 Win32 控制台时，控制台子系统子进程会被分配新控制台，即
//   黑框闪屏），绝不用 std::system/_popen（会调起 cmd.exe，同样弹窗且继承句柄），日志自写文件。
// 绝不继承句柄（bInheritHandles=FALSE）：ctest 通过管道捕获 stdout，长驻子进程若继承了该管道
//   写端，运行器会永远等不到 EOF 而挂死。停止用 TerminateProcess（CreateProcess 的 PID），
//   不用 taskkill/netstat（会弹窗）。

#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace pjf_test {

// 分离启动子进程；返回 PID（0 = 启动失败）。子进程无窗口、不继承句柄、不等待其结束。
inline unsigned long spawn_detached(const std::string& cmdline) {
#ifdef _WIN32
    std::vector<char> buf(cmdline.begin(), cmdline.end());
    buf.push_back('\0');
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, buf.data(), nullptr, nullptr,
                             /*bInheritHandles=*/FALSE,
                             CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi);
    if (!ok) return 0;
    CloseHandle(pi.hThread);
    unsigned long pid = static_cast<unsigned long>(pi.dwProcessId);
    CloseHandle(pi.hProcess);
    return pid;
#else
    // POSIX：交给 shell 后台执行（无窗口与句柄继承问题）
    std::string c = cmdline + " >/dev/null 2>&1 &";
    return static_cast<unsigned long>(std::system(c.c_str()));
#endif
}

// 结束指定 PID 的进程（不存在/已退出则静默返回）
inline void terminate_pid(unsigned long pid) {
    if (pid == 0) return;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!h) return;
    TerminateProcess(h, 0);
    WaitForSingleObject(h, 2000);
    CloseHandle(h);
#else
    (void)pid;
#endif
}

} // namespace pjf_test

#endif // PJFETCH_TEST_SPAWN_HPP
