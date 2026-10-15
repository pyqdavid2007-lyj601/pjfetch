// pjf_test_server —— 测试用本机 HTTP 服务器（独立进程）。以 Windows(GUI) 子系统编译（见
// test/CMakeLists.txt 的 WIN32_EXECUTABLE），操作系统不会为它分配控制台，"测试绝不弹窗"由此保证。
// 用法：--port N [--log FILE] [--size BYTES] [--seed S] [--no-range] [--collapse]
//   [--status CODE] [--throttle KBPS] [--redirect] [--kill-after BYTES] [--ttl SECONDS]
// 就绪标志：日志出现 "range_server on"（与 range_server.py 一致，便于替换）。

#include "http_server_impl.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

namespace {

std::string arg_value(int argc, char** argv, const char* name, const char* dflt) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    return dflt ? dflt : "";
}

bool has_flag(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return true;
    return false;
}

} // namespace

int main(int argc, char** argv) {
    const int port = std::atoi(arg_value(argc, argv, "--port", "8000").c_str());

    std::ofstream log_file;
    std::ostream* log = &std::cout;
    std::string log_path = arg_value(argc, argv, "--log", "");
    if (!log_path.empty()) {
        log_file.open(log_path, std::ios::app);
        if (log_file) log = &log_file;
    }

    pjf_srv::Config cfg;
    cfg.size = std::strtoll(arg_value(argc, argv, "--size", "1048576").c_str(), nullptr, 10);
    cfg.seed = static_cast<uint32_t>(std::strtoul(arg_value(argc, argv, "--seed", "42").c_str(), nullptr, 10));
    cfg.no_range = has_flag(argc, argv, "--no-range");
    cfg.collapse = has_flag(argc, argv, "--collapse");
    cfg.status = std::atoi(arg_value(argc, argv, "--status", "0").c_str());
    cfg.throttle_kbps = std::atoi(arg_value(argc, argv, "--throttle", "0").c_str());
    cfg.redirect = has_flag(argc, argv, "--redirect");
    cfg.kill_after = std::strtoll(arg_value(argc, argv, "--kill-after", "0").c_str(), nullptr, 10);

    auto srv = pjf_srv::HttpServer::start(port, cfg, log);
    if (!srv) {
        if (log) {
            *log << "range_server failed to bind 127.0.0.1:" << port << std::endl;
            log->flush();
        }
        return 1;
    }

    // TTL 看门狗：到点自退，测试异常结束时也不留僵尸进程
    const double ttl = std::atof(arg_value(argc, argv, "--ttl", "300").c_str());
    if (ttl > 0) {
        std::thread([ttl, srv] {
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(ttl * 1000)));
            std::exit(0);
        }).detach();
    }

    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}
