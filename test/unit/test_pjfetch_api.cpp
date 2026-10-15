// libpjfetch.dll —— C ABI 黑盒单元测试（不需要网络服务器）。

#include "CppUTest/TestHarness.h"

extern "C" {
#include "pjfetch/pjfetch.h"
}

#include <atomic>
#include <cstring>
#include <string>

namespace {

pjf_options base_options(const char* url) {
    pjf_options o{};
    o.url = url;
    o.N_segments = 4;
    o.max_retry = 1;
    o.connect_timeout_ms = 1500;      // 保持测试快速
    o.low_speed_limit = 0;            // 关掉低速检测
    o.min_segment_bytes = 65536;
    return o;
}

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

bool wait_done(pjf_task_t t, uint32_t ms) {
    return pjf_download_wait(t, ms) == PJF_OK;
}

} // namespace

TEST_GROUP(PjfetchApi){};

// --- 版本与创建 ---

TEST(PjfetchApi, version_is_non_empty) {
    const char* v = pjf_version();
    CHECK(v != nullptr);
    CHECK(strlen(v) > 0);
}

TEST(PjfetchApi, create_with_null_options_returns_null) {
    CHECK(pjf_downloader_create(nullptr) == nullptr);
}

TEST(PjfetchApi, create_with_empty_url_returns_null) {
    pjf_options o = base_options("");
    CHECK(pjf_downloader_create(&o) == nullptr);
}

TEST(PjfetchApi, create_with_null_url_returns_null) {
    pjf_options o = base_options(nullptr);
    CHECK(pjf_downloader_create(&o) == nullptr);
}

TEST(PjfetchApi, create_with_unsupported_scheme_returns_null) {
    pjf_options o = base_options("ftp://example.test/f.bin");
    CHECK(pjf_downloader_create(&o) == nullptr);
}

TEST(PjfetchApi, create_with_valid_http_url_succeeds) {
    pjf_options o = base_options("http://127.0.0.1:1/f.bin");
    pjf_downloader_t d = pjf_downloader_create(&o);
    CHECK(d != nullptr);
    pjf_downloader_destroy(d);
}

TEST(PjfetchApi, create_with_https_url_succeeds) {
    pjf_options o = base_options("https://example.test/f.bin");
    pjf_downloader_t d = pjf_downloader_create(&o);
    CHECK(d != nullptr);
    pjf_downloader_destroy(d);
}

TEST(PjfetchApi, destroy_null_is_noop) {
    pjf_downloader_destroy(nullptr);   // 不得崩溃
}

// --- NULL 安全 ---

TEST(PjfetchApi, null_task_queries_are_safe) {
    pjf_progress p{};
    pjf_result r{};
    LONGS_EQUAL(PJF_ERR_INVALID_ARG, pjf_download_wait(nullptr, 0));
    LONGS_EQUAL(-1, pjf_query_segments(nullptr, nullptr, 0));
    LONGS_EQUAL(-1, pjf_get_header(nullptr, "Content-Length", nullptr, 0));
    LONGS_EQUAL(-1, pjf_last_error(nullptr, nullptr, 0));
    pjf_download_cancel(nullptr);
    LONGS_EQUAL(-1, pjf_download_is_running(nullptr));   // 无效句柄 -> -1
    LONGS_EQUAL(PJF_ERR_INVALID_ARG, pjf_download_result(nullptr, &r));
    LONGS_EQUAL(PJF_ERR_INVALID_ARG, pjf_query_progress(nullptr, &p));
}

// --- 失败路径：不可达目标 ---

TEST(PjfetchApi, unreachable_target_fails_with_network_error) {
    pjf_options o = base_options("http://127.0.0.1:1/nope.bin");
    o.output_path = "api_unreachable.bin";
    pjf_downloader_t d = pjf_downloader_create(&o);
    CHECK(d != nullptr);

    pjf_task_t t = pjf_download_start(d, nullptr, nullptr, nullptr, nullptr, nullptr);
    CHECK(t != nullptr);
    CHECK(wait_done(t, 30000));

    pjf_result r{};
    LONGS_EQUAL(PJF_OK, pjf_download_result(t, &r));
    CHECK(r.err != PJF_OK);
    CHECK(r.message[0] != '\0');                       // 有可读诊断
    LONGS_EQUAL(0, pjf_download_is_running(t));

    pjf_downloader_destroy(d);
}

TEST(PjfetchApi, callbacks_do_not_fire_when_task_fails_before_download_phase) {
    // 探测阶段即失败时 sampler 不启动，回调一次也不触发，须由 wait + result 兜底；
    // 回调确实触发的正例见 e2e 组。
    pjf_options o = base_options("http://127.0.0.1:1/nope.bin");
    o.output_path = "api_cb.bin";
    pjf_downloader_t d = pjf_downloader_create(&o);
    CHECK(d != nullptr);

    CbCounters c;
    pjf_task_t t = pjf_download_start(d, nullptr, progress_cb, &c, status_cb, &c);
    CHECK(t != nullptr);
    CHECK(wait_done(t, 30000));
    LONGS_EQUAL(0, c.status.load());
    LONGS_EQUAL(0, c.progress.load());

    pjf_result r{};
    LONGS_EQUAL(PJF_OK, pjf_download_result(t, &r));
    CHECK(r.err != PJF_OK);                            // 失败仍可从 result 读到

    pjf_downloader_destroy(d);
}

TEST(PjfetchApi, query_progress_is_callable_before_and_after_completion) {
    pjf_options o = base_options("http://127.0.0.1:1/nope.bin");
    o.output_path = "api_query.bin";
    pjf_downloader_t d = pjf_downloader_create(&o);
    CHECK(d != nullptr);

    pjf_task_t t = pjf_download_start(d, nullptr, nullptr, nullptr, nullptr, nullptr);
    CHECK(t != nullptr);
    CHECK(wait_done(t, 30000));

    pjf_progress p{};
    LONGS_EQUAL(PJF_OK, pjf_query_progress(t, &p));
    LONGS_EQUAL(0, p.total_segments);                  // 探测失败 -> 未规划分段
    LONGS_EQUAL(0, p.downloaded_bytes);

    pjf_downloader_destroy(d);
}

TEST(PjfetchApi, blocking_convenience_download_reports_failure) {
    pjf_options o = base_options("http://127.0.0.1:1/nope.bin");
    o.output_path = "api_blocking.bin";
    pjf_downloader_t d = pjf_downloader_create(&o);
    CHECK(d != nullptr);

    pjf_result r{};
    pjf_err e = pjf_download(d, &o, nullptr, nullptr, &r);
    // 返回 PJF_OK = "结果已填入 out"；下载自身成败在 r.err（与 pjf_download_result 同语义）
    LONGS_EQUAL(PJF_OK, e);
    CHECK(r.err != PJF_OK);
    CHECK(r.message[0] != '\0');

    pjf_downloader_destroy(d);
}

TEST(PjfetchApi, last_error_is_available_after_failure) {
    pjf_options o = base_options("http://127.0.0.1:1/nope.bin");
    o.output_path = "api_lasterr.bin";
    pjf_downloader_t d = pjf_downloader_create(&o);
    CHECK(d != nullptr);

    pjf_task_t t = pjf_download_start(d, nullptr, nullptr, nullptr, nullptr, nullptr);
    CHECK(wait_done(t, 30000));

    char buf[512];
    int n = pjf_last_error(t, buf, sizeof buf);
    CHECK(n > 0);
    CHECK(buf[0] != '\0');

    pjf_downloader_destroy(d);
}

TEST(PjfetchApi, last_error_is_truncated_safely_and_reports_full_length) {
    pjf_options o = base_options("http://127.0.0.1:1/nope.bin");
    o.output_path = "api_cap.bin";
    pjf_downloader_t d = pjf_downloader_create(&o);
    CHECK(d != nullptr);

    pjf_task_t t = pjf_download_start(d, nullptr, nullptr, nullptr, nullptr, nullptr);
    CHECK(wait_done(t, 30000));

    // 先用大缓冲拿到完整消息
    char big[512];
    int full = pjf_last_error(t, big, sizeof big);
    CHECK(full > 0);

    // 小缓冲：返回仍为完整长度（snprintf 语义），内容被截断且以 '\0' 结尾
    char small[8];
    std::memset(small, '\xAA', sizeof small);          // 预填哨兵，检测越界/未终止
    int n = pjf_last_error(t, small, sizeof small);
    LONGS_EQUAL(full, n);
    LONGS_EQUAL('\0', small[sizeof small - 1]);        // 末尾必须是终止符
    CHECK(strlen(small) <= sizeof small - 1);
    STRCMP_EQUAL(std::string(big, 7).c_str(), std::string(small).c_str());

    pjf_downloader_destroy(d);
}
