#ifndef PJFETCH_HTTP_SERVER_IMPL_HPP
#define PJFETCH_HTTP_SERVER_IMPL_HPP

// 测试用 HTTP 服务器实现（由 test/pjf_test_server.cpp 独立进程使用）；内容模型与
//   test/range_server.py 同构（LCG + 1MiB 周期表），产出字节一致，便于交叉比对。
// 必须独立进程：libgo 的钩子是 Detours 式进程级补丁，改写 ws2_32 的 send/recv/accept/select；
//   同进程内普通 socket 调用会落进协程调度钩子（未定义行为）。GUI 子系统编译天然无控制台窗口。

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
using sock_t = SOCKET;
#  define PJF_CLOSESOCK closesocket
#  define PJF_INVALID_SOCK INVALID_SOCKET
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
using sock_t = int;
#  define PJF_CLOSESOCK close
#  define PJF_INVALID_SOCK (-1)
#endif

namespace pjf_srv {

struct Config {
    int64_t size = 1 << 20;
    uint32_t seed = 42;
    bool no_range = false;
    bool collapse = false;
    int status = 0;
    int throttle_kbps = 0;
    bool redirect = false;
    int64_t kill_after = 0;
};

class HttpServer : public std::enable_shared_from_this<HttpServer> {
public:
    static std::shared_ptr<HttpServer> start(int port, const Config& cfg, std::ostream* log) {
        auto s = std::shared_ptr<HttpServer>(new HttpServer());
        s->log_ = log;
        if (!s->init(port, cfg)) return nullptr;
        return s;
    }

    ~HttpServer() { stop(); }

    void stop() {
        if (listen_sock_ == PJF_INVALID_SOCK) return;
        PJF_CLOSESOCK(listen_sock_);
        listen_sock_ = PJF_INVALID_SOCK;
        if (accept_thread_.joinable()) accept_thread_.join();
    }

private:
    HttpServer() = default;

    void say(const std::string& s) {
        if (log_) {
            std::lock_guard<std::mutex> lk(log_m_);
            *log_ << s << '\n';
            log_->flush();
        }
    }

    bool init(int port, const Config& cfg) {
#ifdef _WIN32
        static std::once_flag wsa_once;
        std::call_once(wsa_once, [] {
            WSADATA d;
            WSAStartup(MAKEWORD(2, 2), &d);
        });
#endif
        cfg_ = cfg;
        port_ = port;
        kill_armed_ = cfg.kill_after > 0;
        table_ = build_table(cfg.seed);

        listen_sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_sock_ == PJF_INVALID_SOCK) return false;
        int one = 1;
        ::setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&one), sizeof one);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<unsigned short>(port));
        if (::bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
            PJF_CLOSESOCK(listen_sock_);
            listen_sock_ = PJF_INVALID_SOCK;
            return false;
        }
        if (::listen(listen_sock_, 64) != 0) {
            PJF_CLOSESOCK(listen_sock_);
            listen_sock_ = PJF_INVALID_SOCK;
            return false;
        }
        auto self = shared_from_this();
        accept_thread_ = std::thread([this, self] { accept_loop(self); });
        say("range_server on 127.0.0.1:" + std::to_string(port) +
            " size=" + std::to_string(cfg.size) +
            " no_range=" + (cfg.no_range ? "1" : "0") +
            " collapse=" + (cfg.collapse ? "1" : "0") +
            " status=" + std::to_string(cfg.status) +
            " throttle=" + std::to_string(cfg.throttle_kbps) +
            " redirect=" + (cfg.redirect ? "1" : "0") +
            " kill_after=" + std::to_string(cfg.kill_after));
        return true;
    }

    static std::vector<unsigned char> build_table(uint32_t seed) {
        const uint32_t A = 1103515245u, C = 12345u;
        std::vector<unsigned char> t(size_t(1) << 20);
        uint32_t x = seed;
        for (size_t i = 0; i < t.size(); ++i) {
            x = A * x + C;
            t[i] = static_cast<unsigned char>((x >> 16) & 0xFF);
        }
        return t;
    }

    void accept_loop(std::shared_ptr<HttpServer> self) {
        while (listen_sock_ != PJF_INVALID_SOCK) {
            sock_t c = ::accept(listen_sock_, nullptr, nullptr);
            if (c == PJF_INVALID_SOCK) break;
            std::thread([this, c, self] { handle(c); }).detach();
        }
    }

    bool send_all(sock_t c, const char* data, size_t len) {
#ifdef _WIN32
        while (len > 0) {
            int n = ::send(c, data, static_cast<int>(len), 0);
            if (n <= 0) return false;
            data += n;
            len -= static_cast<size_t>(n);
        }
#else
        while (len > 0) {
            ssize_t n = ::send(c, data, len, MSG_NOSIGNAL);
            if (n <= 0) return false;
            data += n;
            len -= static_cast<size_t>(n);
        }
#endif
        return true;
    }
    void send_str(sock_t c, const std::string& s) { send_all(c, s.data(), s.size()); }

    void send_content(sock_t c, int64_t start, int64_t length) {
        const auto& t = table_;
        const int64_t b = static_cast<int64_t>(t.size());
        int64_t pos = start, remaining = length;
        std::vector<char> buf(64 * 1024);
        while (remaining > 0) {
            int64_t p = pos % b;
            int64_t take = std::min<int64_t>(remaining, std::min<int64_t>(b - p, 64 * 1024));
            std::memcpy(buf.data(), t.data() + p, static_cast<size_t>(take));
            if (!send_all(c, buf.data(), static_cast<size_t>(take))) return;
            remaining -= take;
            pos += take;
        }
    }

    struct LingerOpt {
        int l_onoff;
        int l_linger;
    };

    bool send_body_maybe_kill(sock_t c, int64_t start, int64_t n) {
        int64_t kill = 0;
        {
            std::lock_guard<std::mutex> lk(kill_m_);
            if (kill_armed_) {
                kill = cfg_.kill_after;
                kill_armed_ = false;
            }
        }
        if (kill <= 0 || kill >= n) {
            send_content(c, start, n);
            return false;
        }
        send_content(c, start, kill);
        LingerOpt lg{1, 0};
        ::setsockopt(c, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&lg), sizeof lg);
        PJF_CLOSESOCK(c);
        return true;
    }

    void send_throttled(sock_t c, bool send_body) {
        send_str(c, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n");
        if (!send_body) return;
        double rate = cfg_.throttle_kbps * 1024.0;
        int64_t sent = 0;
        // 分块要够大：Windows 上 sleep_for 被舍入到 ~15.6ms 定时器精度，按 1KB
        // 分块、每块睡约 2ms 实测会慢近一个数量级。
        const int64_t chunk = 32 * 1024;
        while (sent < cfg_.size) {
            int64_t take = std::min<int64_t>(chunk, cfg_.size - sent);
            const auto& t = table_;
            const int64_t b = static_cast<int64_t>(t.size());
            std::vector<char> buf(static_cast<size_t>(take));
            for (int64_t i = 0; i < take; ++i) buf[static_cast<size_t>(i)] = t[(sent + i) % b];
            if (!send_all(c, buf.data(), buf.size())) return;
            sent += take;
            if (sent < cfg_.size) {
                int ms = static_cast<int>(1000.0 * take / rate);
                std::this_thread::sleep_for(std::chrono::milliseconds(ms > 0 ? ms : 1));
            }
        }
    }

    struct RangeResult {
        bool absent = false;
        bool over = false;
        int64_t start = 0;
        int64_t end = 0;
    };

    RangeResult parse_range(const std::string& raw, int64_t size) {
        RangeResult r;
        if (raw.empty() || raw.rfind("bytes=", 0) != 0) { r.absent = true; return r; }
        std::string spec = raw.substr(6);
        if (spec.find(',') != std::string::npos) { r.absent = true; return r; }
        size_t dash = spec.find('-');
        if (dash == std::string::npos) { r.absent = true; return r; }
        std::string a = spec.substr(0, dash), b = spec.substr(dash + 1);
        try {
            if (a.empty()) {
                long long n = std::stoll(b);
                if (n <= 0) { r.absent = true; return r; }
                r.start = std::max<int64_t>(0, size - n);
                r.end = size - 1;
                return r;
            }
            long long s = std::stoll(a);
            if (s >= size) { r.over = true; return r; }
            r.start = s;
            r.end = b.empty() ? size - 1 : std::min<long long>(std::stoll(b), size - 1);
            if (r.start > r.end) r.absent = true;
        } catch (...) {
            r.absent = true;
        }
        return r;
    }

    void handle(sock_t c) {
        bool closed = false;
        std::string req;
        char buf[4096];
        while (req.find("\r\n\r\n") == std::string::npos) {
            int n = ::recv(c, buf, sizeof buf, 0);
            if (n <= 0) { PJF_CLOSESOCK(c); return; }
            req.append(buf, static_cast<size_t>(n));
            if (req.size() > 64 * 1024) break;
        }

        std::string method, path, range;
        {
            size_t eol = req.find("\r\n");
            std::string line = req.substr(0, eol);
            size_t sp1 = line.find(' ');
            size_t sp2 = line.find(' ', sp1 + 1);
            method = line.substr(0, sp1);
            path = (sp2 == std::string::npos) ? line.substr(sp1 + 1)
                                              : line.substr(sp1 + 1, sp2 - sp1 - 1);
            std::string lower = req;
            for (auto& ch : lower) ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
            size_t rp = lower.find("\r\nrange:");
            if (rp != std::string::npos) {
                size_t vs = lower.find(':', rp + 2) + 1;
                size_t ve = req.find("\r\n", vs);
                std::string v = req.substr(vs, ve - vs);
                size_t b = v.find_first_not_of(" \t");
                size_t e = v.find_last_not_of(" \t\r");
                if (b != std::string::npos && e != std::string::npos && e >= b)
                    range = v.substr(b, e - b + 1);
            }
        }
        bool send_body = (method != "HEAD");
        say(method + " " + path + " range=" + (range.empty() ? "-" : range));

        if (cfg_.status) {
            send_str(c, "HTTP/1.1 " + std::to_string(cfg_.status) + " X\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            PJF_CLOSESOCK(c);
            return;
        }
        if (cfg_.redirect && path.rfind("/real", 0) != 0) {
            send_str(c, "HTTP/1.1 302 Found\r\nLocation: /real\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            PJF_CLOSESOCK(c);
            return;
        }

        RangeResult r;
        if (cfg_.no_range || cfg_.throttle_kbps > 0)
            r.absent = true;
        else
            r = parse_range(range, cfg_.size);

        if (cfg_.collapse && !r.absent && !r.over) {
            bool is_probe = (r.start == 0 && r.end == 0);
            int seen = 0;
            {
                std::lock_guard<std::mutex> lk(collapse_m_);
                seen = collapse_seen_;
                if (!is_probe) ++collapse_seen_;
            }
            if (!is_probe && seen >= 1) r.absent = true;
        }

        if (r.over) {
            send_str(c, "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" +
                            std::to_string(cfg_.size) + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            PJF_CLOSESOCK(c);
            return;
        }
        if (r.absent) {
            if (cfg_.throttle_kbps > 0) {
                send_throttled(c, send_body);
            } else {
                send_str(c, "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(cfg_.size) +
                                "\r\nConnection: close\r\n\r\n");
                if (send_body) closed = send_body_maybe_kill(c, 0, cfg_.size);
            }
            if (!closed) PJF_CLOSESOCK(c);
            return;
        }
        int64_t n = r.end - r.start + 1;
        send_str(c, "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " +
                        std::to_string(r.start) + "-" + std::to_string(r.end) + "/" +
                        std::to_string(cfg_.size) + "\r\nContent-Length: " + std::to_string(n) +
                        "\r\nConnection: close\r\n\r\n");
        if (send_body) closed = send_body_maybe_kill(c, r.start, n);
        if (!closed) PJF_CLOSESOCK(c);
    }

    Config cfg_;
    int port_ = 0;
    sock_t listen_sock_ = PJF_INVALID_SOCK;
    std::thread accept_thread_;
    std::vector<unsigned char> table_;
    std::ostream* log_ = nullptr;
    std::mutex log_m_;
    std::mutex collapse_m_;
    int collapse_seen_ = 0;
    std::mutex kill_m_;
    bool kill_armed_ = false;
};

} // namespace pjf_srv

#endif // PJFETCH_HTTP_SERVER_IMPL_HPP
