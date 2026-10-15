// libpjfetch 内部单元白盒测试：分段规划边界 / sidecar 往返与恢复准入 / 错误码映射 / 文件写入接缝
// （这些 .cpp 直接编入本测试可执行文件，函数未导出，与 DLL 内副本不冲突）。

#include "CppUTest/TestHarness.h"

#include "downloader_impl.hpp"

#include <cpr/error.h>          // cpr::ErrorCode 真值（避免在测试里硬编数字）

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using pjf::FileWriter;
using pjf::MetaData;
using pjf::TaskOptions;
using pjf::plan_segments;

namespace {

std::filesystem::path tmp() {
    // 写入系统临时目录而非源码树：跑完测试后源码目录保持干净
    std::error_code ec0;
    static std::filesystem::path base =
        std::filesystem::temp_directory_path(ec0) / "pjfetch_tests" / "units";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    return base;
}

std::string write_file(const std::string& name, const std::string& content) {
    auto p = tmp() / name;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << content;
    return p.string();
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string fresh_path(const char* name) {
    auto p = tmp() / name;
    std::error_code ec;
    std::filesystem::remove(p, ec);
    return p.string();
}

TaskOptions opts_with(int32_t segments, int64_t min_seg) {
    TaskOptions o;
    o.segments = segments;
    o.min_segment_bytes = min_seg;
    return o;
}

} // namespace

TEST_GROUP(Planner){};

TEST(Planner, no_range_support_yields_single_stream_segment) {
    // end=-1 是"流式读到底 + 不发 Range 头"的标记；即便总大小已知也不填 total-1，
    // 否则会向不支持 Range 的服务器发 Range 请求。
    auto v = plan_segments(10 * 1024 * 1024, /*range_ok=*/false, opts_with(8, 1 << 20));
    LONGS_EQUAL(1, (long)v.size());
    LONGS_EQUAL(0, (long)v[0].first);
    LONGS_EQUAL(-1, (long)v[0].second);
}

TEST(Planner, unknown_total_yields_single_stream_segment) {
    auto v = plan_segments(-1, true, opts_with(8, 1 << 20));
    LONGS_EQUAL(1, (long)v.size());
    LONGS_EQUAL(0, (long)v[0].first);
    LONGS_EQUAL(-1, (long)v[0].second);          // end=-1 表示未知大小
}

TEST(Planner, file_smaller_than_two_min_segments_stays_single) {
    auto v = plan_segments(1 << 20, true, opts_with(8, 1 << 20));   // 恰好 1x
    LONGS_EQUAL(1, (long)v.size());
}

TEST(Planner, spans_are_contiguous_and_cover_entire_file) {
    const int64_t total = 100 * 1024 * 1024;
    auto v = plan_segments(total, true, opts_with(8, 1 << 20));
    LONGS_EQUAL(8, (long)v.size());

    LONGS_EQUAL(0, (long)v.front().first);
    LONGS_EQUAL(total - 1, (long)v.back().second);
    for (size_t i = 0; i < v.size(); ++i) {
        CHECK(v[i].first <= v[i].second);
        if (i > 0) LONGS_EQUAL((long)(v[i - 1].second + 1), (long)v[i].first);   // 无缝隙
    }
    int64_t sum = 0;
    for (auto& s : v) sum += s.second - s.first + 1;
    LONGS_EQUAL((long)total, (long)sum);
}

TEST(Planner, sizes_are_balanced_within_one_byte) {
    const int64_t total = 1000 * 1000 * 1000;    // 不能被 8 整除
    auto v = plan_segments(total, true, opts_with(8, 1 << 20));
    int64_t mn = INT64_MAX, mx = 0;
    for (auto& s : v) {
        int64_t n = s.second - s.first + 1;
        if (n < mn) mn = n;
        if (n > mx) mx = n;
    }
    CHECK(mx - mn <= 1);
}

TEST(Planner, segment_count_limited_by_min_segment_bytes) {
    auto v = plan_segments(8 * 1024 * 1024, true, opts_with(8, 4 * 1024 * 1024));
    LONGS_EQUAL(2, (long)v.size());
}

TEST_GROUP(Meta){};

TEST(Meta, save_then_load_round_trips_all_fields) {
    MetaData m;
    m.version = 1;
    m.url = "http://example.test/f.bin";
    m.etag = "\"abc123\"";
    m.last_modified = "Wed, 21 Oct 2026 07:28:00 GMT";
    m.total = 123456789;
    m.segments = 4;
    m.range_mode = 1;
    m.seg_done = {100, 200, 300, 400};
    m.seg_finished = {1, 0, 0, 1};

    std::string path = fresh_path("roundtrip.pjmeta");
    CHECK(pjf::meta_save(path, m));

    auto got = pjf::meta_load(path);
    CHECK(got.has_value());
    LONGS_EQUAL(1, got->version);
    STRCMP_EQUAL(m.url.c_str(), got->url.c_str());
    STRCMP_EQUAL(m.etag.c_str(), got->etag.c_str());
    STRCMP_EQUAL(m.last_modified.c_str(), got->last_modified.c_str());
    LONGS_EQUAL((long)m.total, (long)got->total);
    LONGS_EQUAL(4, got->segments);
    LONGS_EQUAL(1, got->range_mode);
    LONGS_EQUAL(4, (long)got->seg_done.size());
    LONGS_EQUAL(400, (long)got->seg_done[3]);
    LONGS_EQUAL(1, got->seg_finished[0]);
    LONGS_EQUAL(0, got->seg_finished[1]);
}

TEST(Meta, load_missing_file_returns_nullopt) {
    auto got = pjf::meta_load(fresh_path("does_not_exist.pjmeta"));
    CHECK_FALSE(got.has_value());
}

TEST(Meta, load_garbage_returns_nullopt) {
    std::string p = write_file("garbage.pjmeta", "this is not a key=value sidecar\n%%%\n");
    auto got = pjf::meta_load(p);
    CHECK_FALSE(got.has_value());
}

TEST(Meta, save_is_atomic_no_tmp_left_behind) {
    MetaData m;
    m.url = "http://x/";
    std::string path = fresh_path("atomic.pjmeta");
    CHECK(pjf::meta_save(path, m));
    CHECK(std::filesystem::exists(path));
    CHECK_FALSE(std::filesystem::exists(path + ".tmp"));
}

TEST(Meta, save_overwrites_existing_sidecar) {
    MetaData m1;
    m1.url = "http://one/";
    m1.total = 10;
    m1.segments = 1;                  // meta_load 要求 segments > 0，否则视为无效 sidecar
    m1.seg_done = {5};
    std::string path = fresh_path("overwrite.pjmeta");
    CHECK(pjf::meta_save(path, m1));

    MetaData m2;
    m2.url = "http://two/";
    m2.total = 20;
    m2.segments = 1;
    m2.seg_done = {7};
    CHECK(pjf::meta_save(path, m2));

    auto got = pjf::meta_load(path);
    CHECK(got.has_value());
    STRCMP_EQUAL("http://two/", got->url.c_str());
    LONGS_EQUAL(20, (long)got->total);
    LONGS_EQUAL(7, (long)got->seg_done[0]);
}

TEST(Meta, load_rejects_sidecar_without_segments) {
    MetaData m;
    m.url = "http://h/f.bin";
    m.total = 100;
    m.segments = 0;
    std::string path = fresh_path("nosegs.pjmeta");
    CHECK(pjf::meta_save(path, m));
    CHECK_FALSE(pjf::meta_load(path).has_value());
}

TEST(Meta, load_pads_short_segment_arrays_to_declared_count) {
    std::string body = "version=1\nurl=http://h/f.bin\ntotal=100\nsegments=3\nseg0.done=10\n";
    std::string path = write_file("padded.pjmeta", body);
    auto got = pjf::meta_load(path);
    CHECK(got.has_value());
    LONGS_EQUAL(3, (long)got->seg_done.size());
    LONGS_EQUAL(10, (long)got->seg_done[0]);
    LONGS_EQUAL(0, (long)got->seg_done[2]);
}

// --- 恢复准入规则 ---

namespace {
MetaData good_meta(const std::string& url, int64_t total) {
    MetaData m;
    m.url = url;
    m.total = total;
    m.segments = 2;
    m.seg_done = {1000, 1000};
    m.seg_finished = {0, 0};
    return m;
}
std::string why;
} // namespace

TEST(Meta, validate_accepts_matching_state) {
    why.clear();
    // .part 按设计预分配为 total 大小，故 part_size 应 >= total
    MetaData m = good_meta("http://h/f.bin", 5000);
    CHECK(pjf::meta_validate_resume(m, "http://h/f.bin", /*part_size=*/5000, "", "", &why));
}

TEST(Meta, validate_accepts_part_larger_than_total) {
    why.clear();
    MetaData m = good_meta("http://h/f.bin", 5000);
    CHECK(pjf::meta_validate_resume(m, "http://h/f.bin", /*part_size=*/6000, "", "", &why));
}

TEST(Meta, validate_rejects_different_url) {
    why.clear();
    MetaData m = good_meta("http://h/f.bin", 5000);
    CHECK_FALSE(pjf::meta_validate_resume(m, "http://other/f.bin", 5000, "", "", &why));
    CHECK(why.size() > 0);
}

TEST(Meta, validate_rejects_unknown_total) {
    why.clear();
    MetaData m = good_meta("http://h/f.bin", -1);    // 未知大小不可续传
    CHECK_FALSE(pjf::meta_validate_resume(m, "http://h/f.bin", 5000, "", "", &why));
    CHECK(why.size() > 0);
}

TEST(Meta, validate_rejects_part_smaller_than_total) {
    why.clear();
    MetaData m = good_meta("http://h/f.bin", 5000);
    CHECK_FALSE(pjf::meta_validate_resume(m, "http://h/f.bin", /*part_size=*/4999, "", "", &why));
    CHECK(why.size() > 0);
}

TEST(Meta, validate_rejects_when_recorded_progress_exceeds_part) {
    why.clear();
    // sum(seg_done)=2000 而 .part 只有 100 字节 -> 判定为崩溃截断，拒绝续传
    MetaData m = good_meta("http://h/f.bin", 5000);
    CHECK_FALSE(pjf::meta_validate_resume(m, "http://h/f.bin", /*part_size=*/100, "", "", &why));
    CHECK(why.size() > 0);
}

TEST(Meta, validate_rejects_when_recorded_progress_exceeds_total) {
    why.clear();
    MetaData m = good_meta("http://h/f.bin", 5000);
    m.seg_done = {9000, 9000};                       // 记录进度超过总长（损坏）
    CHECK_FALSE(pjf::meta_validate_resume(m, "http://h/f.bin", 5000, "", "", &why));
}

TEST(Meta, validate_uses_etag_when_present) {
    why.clear();
    MetaData m = good_meta("http://h/f.bin", 5000);
    m.etag = "\"old\"";
    CHECK_FALSE(pjf::meta_validate_resume(m, "http://h/f.bin", 5000, "\"new\"", "", &why));

    MetaData m2 = good_meta("http://h/f.bin", 5000);
    m2.etag = "\"same\"";
    CHECK(pjf::meta_validate_resume(m2, "http://h/f.bin", 5000, "\"same\"", "", &why));
}

TEST(Meta, validate_falls_back_to_last_modified_without_etag) {
    why.clear();
    MetaData m = good_meta("http://h/f.bin", 5000);
    m.last_modified = "Wed, 21 Oct 2026 07:28:00 GMT";
    CHECK(pjf::meta_validate_resume(m, "http://h/f.bin", 5000, "", m.last_modified, &why));

    MetaData m2 = good_meta("http://h/f.bin", 5000);
    m2.last_modified = "Wed, 21 Oct 2026 07:28:00 GMT";
    CHECK_FALSE(pjf::meta_validate_resume(m2, "http://h/f.bin", 5000, "", "Thu, 22 Oct 2026 00:00:00 GMT", &why));
}

TEST_GROUP(ErrcMap){};

TEST(ErrcMap, ok_maps_to_ok) {
    LONGS_EQUAL(PJF_OK, pjf::map_cpr_error((int)cpr::ErrorCode::OK));
}

TEST(ErrcMap, timeout_maps_to_timeout) {
    LONGS_EQUAL(PJF_ERR_TIMEOUT, pjf::map_cpr_error((int)cpr::ErrorCode::OPERATION_TIMEDOUT));
}

TEST(ErrcMap, url_problems_map_to_bad_url) {
    LONGS_EQUAL(PJF_ERR_BAD_URL, pjf::map_cpr_error((int)cpr::ErrorCode::UNSUPPORTED_PROTOCOL));
    LONGS_EQUAL(PJF_ERR_BAD_URL, pjf::map_cpr_error((int)cpr::ErrorCode::URL_MALFORMAT));
}

TEST(ErrcMap, range_error_maps_to_range_unsupported) {
    LONGS_EQUAL(PJF_ERR_RANGE_UNSUPPORTED, pjf::map_cpr_error((int)cpr::ErrorCode::RANGE_ERROR));
}

TEST(ErrcMap, transport_failures_map_to_network) {
    LONGS_EQUAL(PJF_ERR_NETWORK, pjf::map_cpr_error((int)cpr::ErrorCode::COULDNT_CONNECT));
    LONGS_EQUAL(PJF_ERR_NETWORK, pjf::map_cpr_error((int)cpr::ErrorCode::COULDNT_RESOLVE_HOST));
    LONGS_EQUAL(PJF_ERR_NETWORK, pjf::map_cpr_error((int)cpr::ErrorCode::RECV_ERROR));
    LONGS_EQUAL(PJF_ERR_NETWORK, pjf::map_cpr_error((int)cpr::ErrorCode::PARTIAL_FILE));
    LONGS_EQUAL(PJF_ERR_NETWORK, pjf::map_cpr_error((int)cpr::ErrorCode::SSL_CONNECT_ERROR));
}

TEST(ErrcMap, http_retryable_only_for_429_and_5xx) {
    CHECK(pjf::http_retryable(429));
    CHECK(pjf::http_retryable(500));
    CHECK(pjf::http_retryable(503));
    CHECK_FALSE(pjf::http_retryable(400));
    CHECK_FALSE(pjf::http_retryable(404));
    CHECK_FALSE(pjf::http_retryable(206));
    CHECK_FALSE(pjf::http_retryable(200));
}

TEST(ErrcMap, classify_http_reports_http_status_error) {
    LONGS_EQUAL(PJF_ERR_HTTP_STATUS, pjf::classify_http(404));
    LONGS_EQUAL(PJF_ERR_HTTP_STATUS, pjf::classify_http(500));
}

TEST(ErrcMap, every_err_has_a_human_readable_string) {
    for (int e = PJF_OK; e <= PJF_ERR_INTERNAL; ++e) {
        const char* s = pjf::pjf_err_str((pjf_err)e);
        CHECK(s != nullptr);
        CHECK(strlen(s) > 0);
    }
}

TEST_GROUP(FileWriterTest) {
    std::string path;
    void setup() override { path = fresh_path("writer.part"); }
    void teardown() override {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

TEST(FileWriterTest, preallocates_part_to_known_total) {
    FileWriter w;
    CHECK(w.open_part(path, 4096));
    w.close_all();
    LONGS_EQUAL(4096, (long)std::filesystem::file_size(path));
}

TEST(FileWriterTest, unknown_total_does_not_preallocate) {
    FileWriter w;
    CHECK(w.open_part(path, -1));
    w.close_all();
    CHECK(std::filesystem::exists(path));
}

TEST(FileWriterTest, segments_write_to_independent_offsets) {
    FileWriter w;
    CHECK(w.open_part(path, 300));
    CHECK(w.open_segment(0, 0));
    CHECK(w.open_segment(1, 100));
    CHECK(w.open_segment(2, 200));

    const std::string a(100, 'A'), b(100, 'B'), c(100, 'C');
    CHECK(w.write(0, a.data(), a.size()));
    CHECK(w.write(1, b.data(), b.size()));
    CHECK(w.write(2, c.data(), c.size()));
    w.close_all();

    std::string got = read_file(path);
    LONGS_EQUAL(300, (long)got.size());
    STRCMP_EQUAL(std::string(100, 'A').c_str(), got.substr(0, 100).c_str());
    STRCMP_EQUAL(std::string(100, 'B').c_str(), got.substr(100, 100).c_str());
    STRCMP_EQUAL(std::string(100, 'C').c_str(), got.substr(200, 100).c_str());
}

TEST(FileWriterTest, restart_truncates_and_reopens_as_single_stream) {
    FileWriter w;
    CHECK(w.open_part(path, 300));
    CHECK(w.open_segment(0, 0));
    CHECK(w.write(0, std::string(300, 'X').data(), 300));
    CHECK(w.restart_stream(path));               // collapse 重启

    const std::string fresh(50, 'Z');
    CHECK(w.write(0, fresh.data(), fresh.size()));
    w.close_all();

    std::string got = read_file(path);
    LONGS_EQUAL(50, (long)got.size());
    STRCMP_EQUAL(std::string(50, 'Z').c_str(), got.c_str());
}

TEST(FileWriterTest, open_part_keep_preserves_existing_content) {
    {
        FileWriter w;
        CHECK(w.open_part(path, 100));
        CHECK(w.open_segment(0, 0));
        CHECK(w.write(0, std::string(100, 'K').data(), 100));
        w.close_all();
    }
    FileWriter w2;
    CHECK(w2.open_part_keep(path));              // 续传：不截断
    CHECK(w2.open_segment(0, 40));
    CHECK(w2.write(0, std::string(60, 'M').data(), 60));
    w2.close_all();

    std::string got = read_file(path);
    LONGS_EQUAL(100, (long)got.size());
    STRCMP_EQUAL(std::string(40, 'K').c_str(), got.substr(0, 40).c_str());
    STRCMP_EQUAL(std::string(60, 'M').c_str(), got.substr(40, 60).c_str());
}
