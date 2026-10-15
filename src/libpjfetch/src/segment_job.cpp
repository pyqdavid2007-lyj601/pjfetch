// 分段任务：每段一个 job，attempt 0..max_retry 循环。
// 200-collapse：带 Range 请求收到非 206 的 2xx 全量响应时，写回调在首个 body chunk
// 即置共享 collapse 标志并中止（状态行由头回调先行解析得到）。
#include "downloader_impl.hpp"

#include <cpr/cpr.h>

namespace pjf {

namespace {

struct SegAttemptCtx {
    TaskCtx* tc;
    SegmentState* seg;
    size_t idx;
    bool ranged;                     // 本段是否带 Range 请求（单流 end=-1 不带）
    std::atomic<int> hdr_status{0};  // 头回调解析到的最终状态行
};

// 头回调：记录最后一条 "HTTP/x.y CODE" 状态行（重定向逐跳覆盖 => 终点状态）
bool seg_header_cb(std::string_view line, intptr_t ud) {
    auto* sc = reinterpret_cast<SegAttemptCtx*>(ud);
    if (line.size() >= 12 && line.substr(0, 5) == "HTTP/") {
        size_t sp = line.find(' ');
        if (sp != std::string_view::npos && sp + 1 < line.size()) {
            try {
                int code = std::stoi(std::string(line.substr(sp + 1, 3)));
                sc->hdr_status.store(code, std::memory_order_relaxed);
            } catch (...) {
            }
        }
    }
    return true;
}

bool seg_write_cb(std::string_view data, intptr_t ud) {
    auto* sc = reinterpret_cast<SegAttemptCtx*>(ud);
    TaskCtx& tc = *sc->tc;
    if (tc.canceled.load(std::memory_order_relaxed)) return false;
    if (tc.collapse.load(std::memory_order_relaxed)) return false;
    if (tc.fatal.load(std::memory_order_relaxed)) return false;
    int st = sc->hdr_status.load(std::memory_order_relaxed);
    if (sc->ranged && st >= 200 && st < 300 && st != 206) {
        // Range 被服务器忽略：收到全量响应，不能把这些字节写进段内偏移
        tc.collapse.store(true, std::memory_order_relaxed);
        return false;
    }
    if (data.empty()) return true;
    if (!tc.writer.write(sc->idx, data.data(), data.size())) {
        tc.disk_full.store(true, std::memory_order_relaxed);
        return false;
    }
    sc->seg->done.fetch_add(static_cast<int64_t>(data.size()), std::memory_order_relaxed);
    return true;
}

void backoff_sleep(TaskCtx& tc, int attempt) {
    int64_t total = 500LL << attempt;
    if (total > 8000) total = 8000;
    for (int64_t slept = 0; slept < total;) {
        if (tc.canceled.load(std::memory_order_relaxed) ||
            tc.collapse.load(std::memory_order_relaxed) ||
            tc.fatal.load(std::memory_order_relaxed))
            return;
        int64_t slice = total - slept < 100 ? total - slept : 100;
        pjb_sleep_ms(tc.engine, static_cast<uint32_t>(slice));
        slept += slice;
    }
}

} // namespace

void run_segment_job(TaskCtx& tc, size_t seg_index) {
    SegmentState& seg = *tc.segs[seg_index];
    const TaskOptions& opt = tc.opt;
    int attempt = 0;

    while (true) {
        // ---- 1. 退出条件 ----
        if (tc.canceled.load(std::memory_order_relaxed)) return;
        if (tc.fatal.load(std::memory_order_relaxed)) return;
        if (tc.collapse.load(std::memory_order_relaxed) && tc.segs.size() > 1) return;
        if (seg.finished.load(std::memory_order_relaxed)) return;
        int64_t span = seg.span();
        if (span >= 0 && seg.done.load(std::memory_order_relaxed) >= span) {
            seg.finished.store(1, std::memory_order_relaxed);
            return;
        }

        // ---- 2. 新建会话（每次 attempt 独立） ----
        int64_t from = seg.start + seg.done.load(std::memory_order_relaxed);
        SegAttemptCtx ac{&tc, &seg, seg_index, seg.end >= 0, {}};
        auto s = make_session(opt, opt.url, tc.cancel);
        if (ac.ranged) s->SetRange(cpr::Range{from, seg.end});
        s->SetHeaderCallback(cpr::HeaderCallback{seg_header_cb, reinterpret_cast<intptr_t>(&ac)});

        if (!tc.writer.open_segment(seg_index, from)) {
            tc.set_error(PJF_ERR_DISK_FULL, "cannot open segment stream at offset " +
                                              std::to_string(from));
            tc.fatal.store(true, std::memory_order_relaxed);
            return;
        }

        // ---- 3. 传输 ----
        seg.active.store(1, std::memory_order_relaxed);
        cpr::Response r = s->Download(
            cpr::WriteCallback{seg_write_cb, reinterpret_cast<intptr_t>(&ac)});
        seg.active.store(0, std::memory_order_relaxed);
        seg.attempts.store(attempt + 1, std::memory_order_relaxed);
        tc.http_status.store(static_cast<int>(r.status_code), std::memory_order_relaxed);

        // ---- 4. 分类 ----
        // 已凑满整段：无论 curl 报什么都算成功
        if (span >= 0 && seg.done.load(std::memory_order_relaxed) >= span) {
            seg.finished.store(1, std::memory_order_relaxed);
            return;
        }
        // 捕获标志优先（写回调中止路径）
        if (tc.canceled.load(std::memory_order_relaxed)) return;
        if (tc.disk_full.load(std::memory_order_relaxed)) {
            tc.set_error(PJF_ERR_DISK_FULL, "disk write failed (segment " +
                                                std::to_string(seg_index) + ")");
            tc.fatal.store(true, std::memory_order_relaxed);
            return;
        }
        if (tc.collapse.load(std::memory_order_relaxed)) return;  // 编排器负责重启单流

        int status = static_cast<int>(r.status_code);
        auto ec = r.error.code;
        bool retry = false;

        if (ec == cpr::ErrorCode::ABORTED_BY_CALLBACK) {
            // SetCancellationParam 的取消通道
            if (tc.canceled.load(std::memory_order_relaxed)) return;
            tc.set_error(PJF_ERR_CANCELED, "transfer aborted");
            tc.canceled.store(true, std::memory_order_relaxed);
            return;
        }

        if (ec == cpr::ErrorCode::OK) {
            bool ok_status = status >= 200 && status < 300;
            if (ac.ranged) {
                if (ok_status && status != 206) {
                    tc.collapse.store(true, std::memory_order_relaxed);  // 兜底
                    return;
                }
                if (status == 416) {
                    if (seg.done.load(std::memory_order_relaxed) > 0) {
                        // 服务器认为区间已满足：按已满收段
                        seg.done.store(span, std::memory_order_relaxed);
                        seg.finished.store(1, std::memory_order_relaxed);
                        return;
                    }
                    tc.set_error(PJF_ERR_HTTP_STATUS, "416 with empty segment");
                    tc.fatal.store(true, std::memory_order_relaxed);
                    return;
                }
                if (status == 206) {
                    if (seg.done.load(std::memory_order_relaxed) >= span) {
                        seg.finished.store(1, std::memory_order_relaxed);
                        return;
                    }
                    retry = true;  // 206 但提前收尾
                } else if (http_retryable(status)) {
                    retry = true;
                } else {
                    tc.set_error(classify_http(status),
                                 "HTTP " + std::to_string(status) + " on segment " +
                                     std::to_string(seg_index));
                    tc.fatal.store(true, std::memory_order_relaxed);
                    return;
                }
            } else {
                if (ok_status) {
                    seg.finished.store(1, std::memory_order_relaxed);
                    return;
                }
                if (http_retryable(status)) {
                    retry = true;
                } else {
                    tc.set_error(classify_http(status),
                                 "HTTP " + std::to_string(status) + " (single stream)");
                    tc.fatal.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        } else {
            // cpr 错误：canceled/disk_full/collapse 已在上方处理
            pjf_err mapped = map_cpr_error(static_cast<int>(ec));
            if (mapped != PJF_ERR_NETWORK && mapped != PJF_ERR_TIMEOUT) {
                tc.set_error(mapped, "curl error " +
                                         std::to_string(static_cast<int>(ec)) + ": " +
                                         r.error.message);
                tc.fatal.store(true, std::memory_order_relaxed);
                return;
            }
            retry = true;
        }
        if (!retry) return;  // 非重试路径均已 return；防御性兜底

        // ---- 5. 退避重试（从 start+done 续传） ----
        if (attempt >= opt.max_retry) {
            tc.set_error(PJF_ERR_TOO_MANY_RETRIES,
                         "segment " + std::to_string(seg_index) + " failed after " +
                             std::to_string(attempt + 1) + " attempts: " +
                             r.error.message + " (HTTP " + std::to_string(status) + ")");
            tc.fatal.store(true, std::memory_order_relaxed);
            return;
        }
        backoff_sleep(tc, attempt);
        ++attempt;
    }
}

} // namespace pjf
