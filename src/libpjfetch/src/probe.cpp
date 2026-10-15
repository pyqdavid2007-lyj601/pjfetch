// 探测：HEAD 优先；HEAD 被拒或长度未知时 GET + Range{0,0} 兜底。
// Range 支持采用诚实检测：只认亲眼见到的 206 + Content-Range，不信 Accept-Ranges。
#include "downloader_impl.hpp"

#include <cpr/cpr.h>

namespace pjf {

namespace {

// "bytes 0-0/12345" / "bytes */12345" -> total（失败返回 -1）
int64_t parse_content_range_total(const std::string& v) {
    size_t slash = v.rfind('/');
    if (slash == std::string::npos || slash + 1 >= v.size()) return -1;
    try {
        return static_cast<int64_t>(std::stoll(v.substr(slash + 1)));
    } catch (...) {
        return -1;
    }
}

bool header_lookup(const cpr::Header& h, const char* name, std::string* out) {
    auto it = h.find(name);  // cpr::Header 自带大小写不敏感比较
    if (it == h.end()) return false;
    *out = it->second;
    return !out->empty();
}

bool probe_retryable(const ProbeResult& pr) {
    return pr.err == PJF_ERR_NETWORK || pr.err == PJF_ERR_TIMEOUT ||
           http_retryable(pr.http_status);
}

} // namespace

ProbeResult run_probe(TaskCtx& tc) {
    const TaskOptions& opt = tc.opt;
    ProbeResult pr;
    int attempt = 0;

    while (true) {
        if (tc.canceled.load(std::memory_order_relaxed)) {
            pr.err = PJF_ERR_CANCELED;
            pr.message = "canceled before probe";
            return pr;
        }
        pr = ProbeResult{};

        // ---- 第 1 步：HEAD ----
        {
            auto s = make_session(opt, opt.url, tc.cancel);
            cpr::Response r = s->Head();
            pr.http_status = static_cast<int>(r.status_code);
            pr.final_url = r.url.str();
            if (r.error.code == cpr::ErrorCode::OK &&
                (r.status_code == 200 || r.status_code == 206)) {
                std::string cl;
                header_lookup(r.header, "Content-Length", &cl);
                header_lookup(r.header, "ETag", &pr.etag);
                header_lookup(r.header, "Last-Modified", &pr.last_modified);
                header_lookup(r.header, "Content-Type", &pr.content_type);
                header_lookup(r.header, "Content-Disposition", &pr.content_disposition);
                header_lookup(r.header, "Accept-Ranges", &pr.accept_ranges);
                if (!cl.empty()) {
                    try { pr.total = std::stoll(cl); }
                    catch (...) { pr.total = -1; }
                }
                tc.record_headers(pr);
            } else if (r.error.code == cpr::ErrorCode::OK &&
                       (r.status_code == 405 || r.status_code == 501)) {
                // HEAD 被拒：走 GET 兜底
            } else if (r.error.code != cpr::ErrorCode::OK) {
                pr.err = map_cpr_error(static_cast<int>(r.error.code));
                pr.message = "probe HEAD failed: " + r.error.message;
            } else {
                pr.err = classify_http(static_cast<int>(r.status_code));
                pr.message = "probe HEAD status " + std::to_string(r.status_code);
            }
        }

        // ---- 第 2 步：GET + Range{0,0}（长度未知 / HEAD 被拒 / 未经 206 证实）----
        if (pr.err == PJF_OK && !(pr.range_ok)) {
            auto s = make_session(opt, opt.url, tc.cancel);
            s->SetRange(cpr::Range{0, 0});
            // 只关心状态行与头：首个 body chunk 即中止（cpr 语义 = WRITE_ERROR，属预期）
            struct ProbeAbort { bool aborted = false; } pa;
            cpr::Response r = s->Download(cpr::WriteCallback{
                [&pa](std::string_view, intptr_t) -> bool {
                    pa.aborted = true;
                    return false;
                },
                reinterpret_cast<intptr_t>(&pa)});
            int status = static_cast<int>(r.status_code);
            bool intentional_abort = pa.aborted &&
                r.error.code == cpr::ErrorCode::WRITE_ERROR;

            if (r.error.code == cpr::ErrorCode::OK || intentional_abort) {
                pr.http_status = status;
                pr.final_url = r.url.str();
                std::string cr;
                header_lookup(r.header, "Content-Range", &cr);
                if (status == 206) {
                    pr.range_ok = true;
                    int64_t total = parse_content_range_total(cr);
                    if (total >= 0) pr.total = total;
                } else if (status == 200 || status == 203) {
                    pr.range_ok = false;
                    if (pr.total < 0) {
                        std::string cl;
                        if (header_lookup(r.header, "Content-Length", &cl)) {
                            try { pr.total = std::stoll(cl); }
                            catch (...) { pr.total = -1; }
                        }
                    }
                    if (pr.etag.empty()) {
                        std::string v;
                        if (header_lookup(r.header, "ETag", &v)) pr.etag = v;
                    }
                    if (pr.last_modified.empty()) {
                        std::string v;
                        if (header_lookup(r.header, "Last-Modified", &v)) pr.last_modified = v;
                    }
                    if (pr.content_disposition.empty()) {
                        std::string v;
                        if (header_lookup(r.header, "Content-Disposition", &v))
                            pr.content_disposition = v;
                    }
                }
                // 200 无 Content-Length：pr.total 保持 -1 => 未知大小单流
                tc.record_headers(pr);
            } else if (r.error.code == cpr::ErrorCode::ABORTED_BY_CALLBACK) {
                pr.err = PJF_ERR_CANCELED;
                pr.message = "canceled during probe";
            } else {
                pr.err = map_cpr_error(static_cast<int>(r.error.code));
                pr.message = "probe GET failed: " + r.error.message;
            }
        }

        if (pr.err == PJF_OK) {
            pr.ok = true;
            return pr;
        }
        if (!probe_retryable(pr) || attempt >= opt.max_retry) return pr;
        int64_t backoff = 500LL << attempt;
        if (backoff > 8000) backoff = 8000;
        pjb_sleep_ms(tc.engine, static_cast<uint32_t>(backoff));
        ++attempt;
    }
}

} // namespace pjf
