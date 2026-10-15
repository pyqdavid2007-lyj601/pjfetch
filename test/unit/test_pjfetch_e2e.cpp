// libpjfetch 端到端集成测试（对本地测试服务器真实下载）：单段/多段/无 Range 回退/
// 200-collapse/未知大小/404/302/取消与续传/回调/分段归一化/响应头；
// 服务器按 (端口, 参数) 懒启动并复用，进程退出时由静态析构停止。

#include "CppUTest/TestHarness.h"

#include "test_server.hpp"

extern "C" {
#include "pjfetch/pjfetch.h"
}

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

fs::path out_dir() {
    fs::path d = pjf_test::tmp_dir() / "e2e";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

// 清理正式文件 / .part / .pjmeta，避免用例间串味
std::string fresh_out(const char* name) {
    fs::path p = out_dir() / name;
    std::error_code ec;
    fs::remove(p, ec);
    fs::remove(p.string() + ".part", ec);
    fs::remove(p.string() + ".pjmeta", ec);
    return p.string();
}

// 选项宿主：p 持有 url/output 字符串，保证 pjf_options 的 const char* 在用例期间有效
//（直接存临时 std::string 的 c_str() 会悬垂——初版即踩过）。
struct Opt {
    std::string url;
    std::string out;
    pjf_options o{};

    Opt(const std::string& u, const std::string& out_path)
        : url(u), out(out_path) {
        o.url = url.c_str();
        o.output_path = out.c_str();
        o.N_segments = 8;
        o.min_segment_bytes = 16 * 1024;     // 允许 256KiB 文件切成 8 段
        o.max_retry = 3;
        o.connect_timeout_ms = 5000;
        o.low_speed_limit = 0;
        o.engine_min_threads = 4;
        o.engine_max_threads = 8;
    }
    pjf_options& get() { return o; }
};

// 与 range_server.py 完全同构的内容模型：32 位 LCG 生成 1MiB 表后周期重复
std::vector<unsigned char> expected_bytes(int64_t size, uint32_t seed = 42) {
    const uint32_t A = 1103515245u, C = 12345u;
    const size_t BLOCK = size_t(1) << 20;
    std::vector<unsigned char> table(BLOCK);
    uint32_t x = seed;
    for (size_t i = 0; i < BLOCK; ++i) {
        x = A * x + C;
        table[i] = static_cast<unsigned char>((x >> 16) & 0xFF);
    }
    std::vector<unsigned char> out(static_cast<size_t>(size));
    for (size_t i = 0; i < out.size(); ++i) out[i] = table[i % BLOCK];
    return out;
}

std::vector<unsigned char> read_all(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)),
                                      std::istreambuf_iterator<char>());
}

void check_matches_expected(const std::string& path, int64_t size) {
    auto got = read_all(path);
    LONGS_EQUAL((long)size, (long)got.size());
    auto want = expected_bytes(size);
    MEMCMP_EQUAL(want.data(), got.data(), static_cast<size_t>(size));
}

// 服务器按 (端口, 参数) 懒启动并复用，避免每个用例重复拉起
pjf_test::LocalServer& shared_server(int port, const std::string& flags) {
    static std::map<int, pjf_test::LocalServer> cache;
    auto it = cache.find(port);
    if (it == cache.end())
        it = cache.emplace(port, pjf_test::LocalServer::start(port, flags)).first;
    return it->second;
}

constexpr int kPortMulti = 18801;      // 支持 Range 的常规服务器
constexpr int kPortNoRange = 18802;
constexpr int kPortCollapse = 18803;
constexpr int kPortStatus = 18804;     // 恒 404
constexpr int kPortRedirect = 18805;
constexpr int kPortThrottle = 18806;   // 未知大小慢速
constexpr int kPortSlowRange = 18807;  // 限速（取消/续传用）

constexpr int64_t kMultiSize = 262144;   // 256KiB

struct CbCounters {
    std::atomic<int> progress{0};
    std::atomic<int> status{0};
};
void progress_cb(const pjf_progress*, void* ud) {
    static_cast<CbCounters*>(ud)->progress.fetch_add(1);
}
void status_cb(int32_t, pjf_err, int32_t, void* ud) {
    static_cast<CbCounters*>(ud)->status.fetch_add(1);
}

} // namespace

TEST_GROUP(PjfetchE2E){};

// --- 正常下载路径 ---

TEST(PjfetchE2E, single_segment_small_file_is_byte_exact) {
    auto& s = shared_server(18810, "--size 8192");
    std::string out = fresh_out("small.bin");
    Opt opt(s.url() + "/f.bin", out);
    opt.get().N_segments = 2;
    opt.get().min_segment_bytes = 1 << 20;      // 8KiB 文件 -> 单段

    pjf_downloader_t d = pjf_downloader_create(&opt.get());
    CHECK(d != nullptr);
    pjf_result r{};
    LONGS_EQUAL(PJF_OK, pjf_download(d, &opt.get(), nullptr, nullptr, &r));
    LONGS_EQUAL(PJF_OK, (int)r.err);
    LONGS_EQUAL(8192, (long)r.bytes);
    LONGS_EQUAL(1, r.segments_used);
    check_matches_expected(out, 8192);
    pjf_downloader_destroy(d);
}

TEST(PjfetchE2E, multi_segment_download_is_byte_exact) {
    auto& s = shared_server(kPortMulti, "--size 262144");
    std::string out = fresh_out("multi.bin");
    Opt opt(s.url() + "/f.bin", out);

    pjf_downloader_t d = pjf_downloader_create(&opt.get());
    CHECK(d != nullptr);
    pjf_result r{};
    LONGS_EQUAL(PJF_OK, pjf_download(d, &opt.get(), nullptr, nullptr, &r));
    LONGS_EQUAL(PJF_OK, (int)r.err);
    LONGS_EQUAL(8, r.segments_used);
    CHECK(r.range_used == 1);
    LONGS_EQUAL((long)kMultiSize, (long)r.bytes);
    check_matches_expected(out, kMultiSize);
    pjf_downloader_destroy(d);
}

TEST(PjfetchE2E, no_range_server_falls_back_to_single_stream) {
    auto& s = shared_server(kPortNoRange, "--size 131072 --no-range");
    std::string out = fresh_out("norange.bin");
    Opt opt(s.url() + "/f.bin", out);

    pjf_downloader_t d = pjf_downloader_create(&opt.get());
    CHECK(d != nullptr);
    pjf_result r{};
    LONGS_EQUAL(PJF_OK, pjf_download(d, &opt.get(), nullptr, nullptr, &r));
    LONGS_EQUAL(PJF_OK, (int)r.err);
    LONGS_EQUAL(0, r.range_used);
    LONGS_EQUAL(1, r.segments_used);
    check_matches_expected(out, 131072);
    pjf_downloader_destroy(d);
}

TEST(PjfetchE2E, server_ignoring_range_collapses_to_single_stream) {
    auto& s = shared_server(kPortCollapse, "--size 131072 --collapse");
    std::string out = fresh_out("collapse.bin");
    Opt opt(s.url() + "/f.bin", out);

    pjf_downloader_t d = pjf_downloader_create(&opt.get());
    CHECK(d != nullptr);
    pjf_result r{};
    LONGS_EQUAL(PJF_OK, pjf_download(d, &opt.get(), nullptr, nullptr, &r));
    LONGS_EQUAL(PJF_OK, (int)r.err);
    check_matches_expected(out, 131072);
    pjf_downloader_destroy(d);
}

TEST(PjfetchE2E, unknown_size_throttled_stream_completes) {
    auto& s = shared_server(kPortThrottle, "--size 32768 --throttle 256");
    std::string out = fresh_out("stream.bin");
    Opt opt(s.url() + "/f.bin", out);

    pjf_downloader_t d = pjf_downloader_create(&opt.get());
    CHECK(d != nullptr);
    pjf_result r{};
    LONGS_EQUAL(PJF_OK, pjf_download(d, &opt.get(), nullptr, nullptr, &r));
    LONGS_EQUAL(PJF_OK, (int)r.err);
    LONGS_EQUAL(1, r.segments_used);
    check_matches_expected(out, 32768);
    pjf_downloader_destroy(d);
}

TEST(PjfetchE2E, redirect_is_followed) {
    auto& s = shared_server(kPortRedirect, "--size 65536 --redirect");
    std::string out = fresh_out("redir.bin");
    Opt opt(s.url() + "/f.bin", out);

    pjf_downloader_t d = pjf_downloader_create(&opt.get());
    CHECK(d != nullptr);
    pjf_result r{};
    LONGS_EQUAL(PJF_OK, pjf_download(d, &opt.get(), nullptr, nullptr, &r));
    LONGS_EQUAL(PJF_OK, (int)r.err);
    check_matches_expected(out, 65536);
    pjf_downloader_destroy(d);
}

// --- 错误路径 ---

TEST(PjfetchE2E, http_404_reports_status_error) {
    auto& s = shared_server(kPortStatus, "--status 404");
    std::string out = fresh_out("notfound.bin");
    Opt opt(s.url() + "/f.bin", out);
    opt.get().max_retry = 0;

    pjf_downloader_t d = pjf_downloader_create(&opt.get());
    CHECK(d != nullptr);
    pjf_result r{};
    LONGS_EQUAL(PJF_OK, pjf_download(d, &opt.get(), nullptr, nullptr, &r));
    LONGS_EQUAL(PJF_ERR_HTTP_STATUS, (int)r.err);
    LONGS_EQUAL(404, r.http_status);
    CHECK_FALSE(fs::exists(out));
    pjf_downloader_destroy(d);
}

// --- 回调（正例：真实下载过程中确实触发） ---

TEST(PjfetchE2E, callbacks_fire_during_real_download) {
    auto& s = shared_server(kPortThrottle, "--size 32768 --throttle 256");
    std::string out = fresh_out("cb.bin");
    Opt opt(s.url() + "/f.bin", out);

    pjf_downloader_t d = pjf_downloader_create(&opt.get());
    CHECK(d != nullptr);
    CbCounters c;
    pjf_task_t t = pjf_download_start(d, nullptr, progress_cb, &c, status_cb, &c);
    CHECK(t != nullptr);
    LONGS_EQUAL(PJF_OK, pjf_download_wait(t, 0));       // 0 = 无限等待
    CHECK(c.progress.load() > 0);
    CHECK(c.status.load() > 0);
    check_matches_expected(out, 32768);
    pjf_downloader_destroy(d);
}

// --- 查询接口 ---

TEST(PjfetchE2E, query_segments_reflects_planned_spans) {
    auto& s = shared_server(kPortMulti, "--size 262144");
    std::string out = fresh_out("segs.bin");
    Opt opt(s.url() + "/f.bin", out);

    pjf_downloader_t d = pjf_downloader_create(&opt.get());
    CHECK(d != nullptr);
    pjf_task_t t = pjf_download_start(d, nullptr, nullptr, nullptr, nullptr, nullptr);
    CHECK(t != nullptr);
    LONGS_EQUAL(PJF_OK, pjf_download_wait(t, 0));

    pjf_segment segs[64];
    int n = pjf_query_segments(t, segs, 64);
    CHECK(n >= 1);
    int64_t covered = 0;
    for (int i = 0; i < n && i < 64; ++i) {
        CHECK(segs[i].start <= segs[i].end);
        covered += segs[i].end - segs[i].start + 1;
    }
    LONGS_EQUAL(262144, (long)covered);                // 分段完整覆盖文件

    LONGS_EQUAL(n, pjf_query_segments(t, nullptr, 0)); // buf=NULL 返回所需数量
    pjf_downloader_destroy(d);
}

TEST(PjfetchE2E, response_headers_are_accessible) {
    auto& s = shared_server(kPortMulti, "--size 262144");
    std::string out = fresh_out("hdr.bin");
    Opt opt(s.url() + "/f.bin", out);

    pjf_downloader_t d = pjf_downloader_create(&opt.get());
    CHECK(d != nullptr);
    pjf_task_t t = pjf_download_start(d, nullptr, nullptr, nullptr, nullptr, nullptr);
    CHECK(t != nullptr);
    LONGS_EQUAL(PJF_OK, pjf_download_wait(t, 0));

    // 服务器会发 Content-Length；调用方不做大小写敏感的假设
    char buf[256];
    int n = pjf_get_header(t, "Content-Length", buf, sizeof buf);
    CHECK(n > 0);
    STRCMP_EQUAL("262144", buf);
    CHECK(pjf_get_header(t, "No-Such-Header", buf, sizeof buf) <= 0);
    CHECK(pjf_get_header(t, "content-length", buf, sizeof buf) > 0);

    pjf_downloader_destroy(d);
}

// --- 分段数归一化（公共 ABI 层验证 0->8 与钳位） ---

TEST(PjfetchE2E, segment_count_zero_normalizes_to_eight) {
    auto& s = shared_server(kPortMulti, "--size 262144");
    std::string out = fresh_out("norm8.bin");
    Opt opt(s.url() + "/f.bin", out);
    opt.get().N_segments = 0;                          // -> 默认 8

    pjf_downloader_t d = pjf_downloader_create(&opt.get());
    CHECK(d != nullptr);
    pjf_result r{};
    LONGS_EQUAL(PJF_OK, pjf_download(d, &opt.get(), nullptr, nullptr, &r));
    LONGS_EQUAL(8, r.segments_used);
    pjf_downloader_destroy(d);
}

TEST(PjfetchE2E, segment_count_above_64_clamps) {
    auto& s = shared_server(kPortMulti, "--size 262144");
    std::string out = fresh_out("norm64.bin");
    Opt opt(s.url() + "/f.bin", out);
    opt.get().N_segments = 4096;                       // -> 钳到 64
    opt.get().min_segment_bytes = 1024;

    pjf_downloader_t d = pjf_downloader_create(&opt.get());
    CHECK(d != nullptr);
    pjf_result r{};
    LONGS_EQUAL(PJF_OK, pjf_download(d, &opt.get(), nullptr, nullptr, &r));
    LONGS_EQUAL(64, r.segments_used);
    pjf_downloader_destroy(d);
}

// --- 取消与断点续传 ---

TEST(PjfetchE2E, cancel_mid_download_then_resume_completes) {
    auto& s = shared_server(kPortSlowRange, "--size 262144 --throttle 64");
    // 注意：路径只解析一次（fresh_out 会删除工作文件，二次调用会毁掉续传状态）
    std::string out = fresh_out("resume.bin");

    // 第一次：启动后取消，应留下 .part/.pjmeta 供续传
    {
        Opt opt(s.url() + "/f.bin", out);
        opt.get().max_retry = 1;
        pjf_downloader_t d = pjf_downloader_create(&opt.get());
        CHECK(d != nullptr);
        pjf_task_t t = pjf_download_start(d, nullptr, nullptr, nullptr, nullptr, nullptr);
        CHECK(t != nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        pjf_download_cancel(t);
        LONGS_EQUAL(PJF_OK, pjf_download_wait(t, 0));
        pjf_result r{};
        LONGS_EQUAL(PJF_OK, pjf_download_result(t, &r));
        LONGS_EQUAL(PJF_ERR_CANCELED, (int)r.err);
        CHECK_FALSE(fs::exists(out));                  // 未完成不产生最终文件
        pjf_downloader_destroy(d);
    }
    CHECK(fs::exists(out + ".part"));                  // 续传状态已保留

    {
        Opt opt(s.url() + "/f.bin", out);
        opt.get().flags |= PJF_FLAG_RESUME;
        pjf_downloader_t d = pjf_downloader_create(&opt.get());
        CHECK(d != nullptr);
        pjf_result r{};
        LONGS_EQUAL(PJF_OK, pjf_download(d, &opt.get(), nullptr, nullptr, &r));
        LONGS_EQUAL(PJF_OK, (int)r.err);
        check_matches_expected(out, 262144);
        CHECK_FALSE(fs::exists(out + ".pjmeta"));      // 成功后清理 sidecar
        pjf_downloader_destroy(d);
    }
}

TEST(PjfetchE2E, cancel_returns_canceled_without_final_file) {
    auto& s = shared_server(kPortSlowRange, "--size 262144 --throttle 64");
    std::string out = fresh_out("cancel_only.bin");
    Opt opt(s.url() + "/f.bin", out);

    pjf_downloader_t d = pjf_downloader_create(&opt.get());
    CHECK(d != nullptr);
    pjf_task_t t = pjf_download_start(d, nullptr, nullptr, nullptr, nullptr, nullptr);
    CHECK(t != nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(pjf_download_is_running(t) == 1);
    pjf_download_cancel(t);
    LONGS_EQUAL(PJF_OK, pjf_download_wait(t, 0));

    pjf_result r{};
    LONGS_EQUAL(PJF_OK, pjf_download_result(t, &r));
    LONGS_EQUAL(PJF_ERR_CANCELED, (int)r.err);
    CHECK_FALSE(fs::exists(out));
    pjf_downloader_destroy(d);
}
