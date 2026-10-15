#ifndef PJFETCH_TEST_SERVER_HPP
#define PJFETCH_TEST_SERVER_HPP

// e2e 测试的服务器夹具：以**独立进程**运行 pjf_test_server。
// 为什么必须独立进程：libgo 的 Windows 钩子是 Detours 式**进程级函数补丁**，会改写 ws2_32 的
//   send/recv/accept/select；同进程内服务器的普通 socket 调用会落进这些为"协程调度"设计的
//   钩子，行为未定义（实测随机 SIGSEGV）。
// 为什么不会弹窗、不会挂住 ctest：子进程以 CREATE_NO_WINDOW|DETACHED_PROCESS 且
//   bInheritHandles=FALSE 启动，日志自写文件而不经 shell；另带 TTL 看门狗防僵尸。

#include "test_spawn.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace pjf_test {

inline std::string source_dir() {
#ifdef PJF_TEST_SOURCE_DIR
    return PJF_TEST_SOURCE_DIR;
#else
    return ".";
#endif
}

inline std::filesystem::path tmp_dir() {
    std::error_code ec;
    auto d = std::filesystem::temp_directory_path(ec);
    if (ec) d = std::filesystem::current_path(ec);
    auto sub = d / "pjfetch_tests";
    std::filesystem::create_directories(sub, ec);
    return sub;
}

class LocalServer {
public:
    LocalServer() = default;
    LocalServer(LocalServer&& o) noexcept
        : pid_(o.pid_), port_(o.port_), url_(std::move(o.url_)), log_(std::move(o.log_)) {
        o.pid_ = 0;
        o.port_ = 0;
    }
    LocalServer& operator=(LocalServer&& o) noexcept {
        if (this != &o) {
            stop();
            pid_ = o.pid_;
            port_ = o.port_;
            url_ = std::move(o.url_);
            log_ = std::move(o.log_);
            o.pid_ = 0;
            o.port_ = 0;
        }
        return *this;
    }
    LocalServer(const LocalServer&) = delete;
    LocalServer& operator=(const LocalServer&) = delete;
    ~LocalServer() { stop(); }

    // flags 例："--size 1048576"、"--no-range"、"--status 404"、"--collapse"
    static LocalServer start(int port, const std::string& flags) {
        LocalServer s;
        s.port_ = port;
        s.url_ = "http://127.0.0.1:" + std::to_string(port);
        s.log_ = (tmp_dir() / ("pjf_srv_" + std::to_string(port) + ".log")).string();
        std::error_code ec;
        std::filesystem::remove(s.log_, ec);

        // 用本项目自带的原生测试服务器（GUI 子系统 -> 永不出现控制台窗口）；
        // 不用外部 python：既弹窗，其解释器也会继承/干扰句柄。
#ifdef PJF_TEST_SERVER_EXE
        std::filesystem::path exe = PJF_TEST_SERVER_EXE;
#else
        std::filesystem::path exe =
            std::filesystem::path(source_dir()) / "build" / "msvc-release" / "bin" / "pjf_test_server.exe";
#endif
        std::string cmd = "\"" + exe.string() + "\" --port " + std::to_string(port) +
                          " --ttl 300 --log \"" + s.log_ + "\" " + flags;
        s.pid_ = spawn_detached(cmd);

        // 就绪轮询：日志出现启动横幅即为已监听（最多 ~10s）
        for (int i = 0; i < 100; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (s.log_contains("range_server on")) return s;
            if (s.pid_ != 0 && !s.pid_alive()) break;      // 子进程已死，不必再等
        }
        std::printf("[test_server] port %d not ready; log:\n", port);
        s.dump_log();
        return s;
    }

    void stop() {
        if (pid_ != 0) {
            terminate_pid(pid_);
            pid_ = 0;
        }
        port_ = 0;
    }

    std::string url() const { return url_; }
    int port() const { return port_; }
    const std::string& log_path() const { return log_; }

    std::string log_text() const {
        std::ifstream f(log_, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    bool log_contains(const std::string& needle) const {
        return log_text().find(needle) != std::string::npos;
    }
    int count_occurrences(const std::string& needle) const {
        std::string t = log_text();
        int n = 0;
        for (size_t p = t.find(needle); p != std::string::npos; p = t.find(needle, p + needle.size())) ++n;
        return n;
    }
    void dump_log() const {
        std::string t = log_text();
        std::fwrite(t.data(), 1, t.size(), stdout);
    }

private:
    bool pid_alive() const {
#ifdef _WIN32
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid_));
        if (!h) return false;
        DWORD r = WaitForSingleObject(h, 0);
        CloseHandle(h);
        return r == WAIT_TIMEOUT;
#else
        return true;
#endif
    }

    unsigned long pid_ = 0;
    int port_ = 0;
    std::string url_;
    std::string log_;
};

} // namespace pjf_test

#endif // PJFETCH_TEST_SERVER_HPP
