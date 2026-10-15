// libpjfetch 编排器与 C ABI 实现。每个 task = 1 编排器 job
// （probe→plan→submit→汇合→finalize）+ N 分段 job + 1 sampler job（250ms tick、
// EMA 速度、用户回调唯一触发线程）。外部线程以 mutex+cv 等待；回调内禁止 destroy（契约）。
#include "downloader_impl.hpp"

#include <cpr/cpr.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace pjf {

namespace {

void sampler(TaskCtx& tc);

struct TaskImpl {
    TaskCtx ctx;
};

struct DownloaderImpl {
    TaskOptions opt;
    pjb_engine_t engine = nullptr;
    std::mutex tasks_m;
    std::vector<std::unique_ptr<TaskImpl>> tasks;  // destroy 前逐个等待完成
};

inline DownloaderImpl* D(pjf_downloader_t d) { return reinterpret_cast<DownloaderImpl*>(d); }
inline TaskImpl* T(pjf_task_t t) { return reinterpret_cast<TaskImpl*>(t); }

void set_msg(char (&dst)[512], const std::string& src) {
    std::snprintf(dst, sizeof(dst), "%s", src.c_str());
}

// 输出路径推导：显式 > URL 文件名清洗 > download.bin
std::string percent_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int h = hex(s[i + 1]), l = hex(s[i + 2]);
            if (h >= 0 && l >= 0) {
                out.push_back(static_cast<char>(h * 16 + l));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

std::string filename_from_url(const std::string& url) {
    size_t end = url.size();
    size_t hash = url.find('#');
    if (hash != std::string::npos) end = hash;
    std::string u = url.substr(0, end);
    size_t q = u.find('?');
    if (q != std::string::npos) u = u.substr(0, q);
    size_t slash = u.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? u : u.substr(slash + 1);
    name = percent_decode(name);
    // 非法字符替换（Windows 设备名/保留名的完整处理在 cli 的 filename 模块）
    for (auto& c : name) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' ||
            c == '|' || c == '?' || c == '*' || uc < 0x20 || uc == 0x7f)
            c = '_';
    }
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    if (name.empty() || name == "." || name == "..") return "download.bin";
    return name;
}

std::string derive_output_path(const TaskOptions& opt) {
    return opt.output_path.empty() ? filename_from_url(opt.url) : opt.output_path;
}

// job thunk（pjb_submit 的 C 回调桥；提交成功后恰好执行一次）
struct SegThunk {
    TaskCtx* tc;
    size_t idx;
};

struct OrchThunk {
    TaskCtx* tc;
};

struct SamplerThunk {
    TaskCtx* tc;
};

void set_phase(TaskCtx& tc, pjf_phase p) {
    tc.phase.store(static_cast<int32_t>(p), std::memory_order_relaxed);
}

int64_t sum_done(const TaskCtx& tc) {
    int64_t s = 0;
    for (auto& seg : tc.segs) s += seg->done.load(std::memory_order_relaxed);
    return s;
}

bool all_segments_finished(const TaskCtx& tc) {
    for (auto& seg : tc.segs)
        if (!seg->finished.load(std::memory_order_relaxed)) return false;
    return true;
}

void write_sidecar(TaskCtx& tc) {
    std::lock_guard<std::mutex> lk(tc.meta_m);
    MetaData m;
    m.version = 1;
    m.url = tc.opt.url;
    m.etag = tc.probe.etag;
    m.last_modified = tc.probe.last_modified;
    m.total = tc.probe.total;
    m.segments = static_cast<int>(tc.segs.size());
    m.range_mode = tc.using_range.load(std::memory_order_relaxed);
    for (auto& seg : tc.segs) {
        m.seg_done.push_back(seg->done.load(std::memory_order_relaxed));
        m.seg_finished.push_back(seg->finished.load(std::memory_order_relaxed));
    }
    meta_save(tc.meta_path, m);
}

// 编排器（跑在一个 parajobs job 里）
void orchestrator(TaskCtx& tc) {
    const TaskOptions& opt = tc.opt;
    pjf_result& res = tc.result;

    auto fail = [&](pjf_err e, const std::string& msg) {
        tc.set_error(e, msg);
        res.err = e;
        set_msg(res.message, msg);
    };
    auto remove_work_files = [&]() {
        std::error_code ec;
        std::filesystem::remove(tc.part_path, ec);
        std::filesystem::remove(tc.meta_path, ec);
    };

    // ---- 探测 ----
    set_phase(tc, PJF_PHASE_PROBE);
    tc.probe = run_probe(tc);
    tc.http_status.store(tc.probe.http_status, std::memory_order_relaxed);
    if (!tc.probe.ok) {
        fail(tc.probe.err != PJF_OK ? tc.probe.err : PJF_ERR_NETWORK, tc.probe.message);
        if (!(opt.flags & PJF_FLAG_RESUME)) remove_work_files();
        goto finalize_error;
    }

    // ---- 规划分段 + 恢复校验 + 工作文件准备 ----
    set_phase(tc, PJF_PHASE_DOWNLOAD);
    {
        auto plan = plan_segments(tc.probe.total, tc.probe.range_ok, opt);
        tc.segs.clear();
        for (auto& pr : plan) {
            auto seg = std::make_unique<SegmentState>();
            seg->start = pr.first;
            seg->end = pr.second;
            tc.segs.push_back(std::move(seg));
        }
        tc.using_range.store(
            (tc.probe.range_ok && tc.probe.total >= 0 && tc.segs[0]->end >= 0) ? 1 : 0,
            std::memory_order_relaxed);
        tc.total_bytes.store(tc.probe.total, std::memory_order_relaxed);

        bool resumed = false;
        std::optional<MetaData> saved;
        if (opt.flags & PJF_FLAG_RESUME) {
            std::error_code ec;
            int64_t part_sz = -1;
            if (std::filesystem::exists(tc.part_path, ec)) {
                auto sz = std::filesystem::file_size(tc.part_path, ec);
                if (!ec) part_sz = static_cast<int64_t>(sz);
            }
            saved = meta_load(tc.meta_path);
            std::string why;
            int want_range = tc.using_range.load(std::memory_order_relaxed);
            if (saved && part_sz >= 0 &&
                meta_validate_resume(*saved, opt.url, part_sz, tc.probe.etag,
                                     tc.probe.last_modified, &why) &&
                saved->total == tc.probe.total && saved->range_mode == want_range &&
                saved->segments == static_cast<int>(tc.segs.size())) {
                resumed = true;
                tc.resumed.store(1, std::memory_order_relaxed);
                for (size_t i = 0; i < tc.segs.size(); ++i) {
                    int64_t span = tc.segs[i]->span();
                    int64_t d = std::max<int64_t>(saved->seg_done[i], 0);
                    if (span >= 0) d = std::min<int64_t>(d, span);  // 显式模板参避开 windows.h 的 min 宏
                    tc.segs[i]->done.store(d, std::memory_order_relaxed);
                    tc.segs[i]->finished.store(span >= 0 && d >= span ? 1 : 0,
                                               std::memory_order_relaxed);
                }
            } else {
                remove_work_files();
            }
        } else {
            remove_work_files();
        }

        bool wr_ok = resumed ? tc.writer.open_part_keep(tc.part_path)
                             : tc.writer.open_part(tc.part_path, tc.probe.total);
        if (!wr_ok) {
            fail(PJF_ERR_DISK_FULL, "cannot prepare " + tc.part_path);
            if (!(opt.flags & PJF_FLAG_RESUME)) remove_work_files();
            goto finalize_error;
        }
        // 关键：提交分段任务前一次性定容段槽位——否则并发 resize 同一 vector 会堆损坏。
        tc.writer.reserve_segments(tc.segs.size());
    }

    // ---- 提交分段 job + sampler job（先计数后提交，防竞态饿死汇合循环） ----
    {
        auto submit_seg = [&tc](size_t idx) -> bool {
            auto* th = new SegThunk{&tc, idx};
            tc.active_jobs.fetch_add(1, std::memory_order_relaxed);
            if (pjb_submit(tc.engine,
                           [](void* ud) {
                               auto* t = static_cast<SegThunk*>(ud);
                               run_segment_job(*t->tc, t->idx);
                               t->tc->active_jobs.fetch_sub(1, std::memory_order_relaxed);
                               delete t;
                           },
                           th, nullptr, nullptr) == PJB_INVALID_JOB) {
                tc.active_jobs.fetch_sub(1, std::memory_order_relaxed);
                delete th;
                return false;
            }
            return true;
        };
        for (size_t i = 0; i < tc.segs.size(); ++i) {
            if (!submit_seg(i)) {
                fail(PJF_ERR_INTERNAL, "pjb_submit failed for segment " + std::to_string(i));
                goto finalize_error;
            }
        }
        if (pjb_submit(tc.engine,
                       [](void* ud) {
                           auto* t = static_cast<SamplerThunk*>(ud);
                           sampler(*t->tc);
                           delete t;
                       },
                       new SamplerThunk{&tc}, nullptr, nullptr) == PJB_INVALID_JOB) {
            fail(PJF_ERR_INTERNAL, "pjb_submit failed for sampler");
            goto finalize_error;
        }

        // ---- 汇合（10ms 协作轮询） ----
        while (tc.active_jobs.load(std::memory_order_relaxed) > 0 &&
               !tc.canceled.load(std::memory_order_relaxed) &&
               !tc.fatal.load(std::memory_order_relaxed)) {
            pjb_sleep_ms(tc.engine, 10);
        }

        // ---- 200-collapse：等分段全部退出后截断重启为单流 ----
        if (tc.collapse.load(std::memory_order_relaxed) &&
            !tc.canceled.load(std::memory_order_relaxed) &&
            !tc.fatal.load(std::memory_order_relaxed)) {
            while (tc.active_jobs.load(std::memory_order_relaxed) > 0)
                pjb_sleep_ms(tc.engine, 10);
            tc.collapse.store(false, std::memory_order_relaxed);
            tc.segs.clear();
            tc.segs.push_back(std::make_unique<SegmentState>());  // 单流：start=0, end=-1
            // 单流：不再发 Range；已知大小仅用于进度显示
            tc.using_range.store(0, std::memory_order_relaxed);
            tc.total_bytes.store(tc.probe.total, std::memory_order_relaxed);
            if (!tc.writer.restart_stream(tc.part_path)) {
                fail(PJF_ERR_DISK_FULL, "cannot restart " + tc.part_path + " as single stream");
                goto finalize_error;
            }
            // 注意：submit_seg 内部已自增 active_jobs，此处不可再自增（重复计数会挂死）。
            if (!submit_seg(0)) {
                fail(PJF_ERR_INTERNAL, "pjb_submit failed for collapsed segment");
                goto finalize_error;
            }
            while (tc.active_jobs.load(std::memory_order_relaxed) > 0 &&
                   !tc.canceled.load(std::memory_order_relaxed) &&
                   !tc.fatal.load(std::memory_order_relaxed)) {
                pjb_sleep_ms(tc.engine, 10);
            }
        }
    }

    // ---- 收尾 ----
    set_phase(tc, PJF_PHASE_FINALIZE);
    tc.writer.close_all();
    tc.speed_bps.store(0.0, std::memory_order_relaxed);

    if (tc.canceled.load(std::memory_order_relaxed)) {
        res.err = PJF_ERR_CANCELED;
        {
            std::lock_guard<std::mutex> lk(tc.err_m);
            set_msg(res.message, tc.err_msg.empty() ? "canceled" : tc.err_msg);
        }
        write_sidecar(tc);  // 保留 .part + sidecar 供 -c 续传
    } else if (tc.fatal.load(std::memory_order_relaxed)) {
        pjf_err e = tc.last_err.load(std::memory_order_relaxed);
        if (e == PJF_OK) e = PJF_ERR_INTERNAL;
        res.err = e;
        {
            std::lock_guard<std::mutex> lk(tc.err_m);
            set_msg(res.message, tc.err_msg.empty() ? pjf_err_str(e) : tc.err_msg);
        }
        write_sidecar(tc);
    } else if (!all_segments_finished(tc)) {
        fail(PJF_ERR_INTERNAL, "segments exited without completion");
        write_sidecar(tc);
    } else {
        // 成功：rename(part→out)、删 sidecar（KEEP_META 除外）
        std::error_code ec;
        std::filesystem::rename(tc.part_path, tc.out_path, ec);
        if (ec) {
            fail(PJF_ERR_IO, "cannot rename " + tc.part_path + " -> " + tc.out_path + ": " +
                                 ec.message());
            write_sidecar(tc);
        } else if (!(opt.flags & PJF_FLAG_KEEP_META)) {
            std::error_code ec2;
            std::filesystem::remove(tc.meta_path, ec2);
        }
    }
    goto finalize_common;

finalize_error:
    set_phase(tc, PJF_PHASE_FINALIZE);
    tc.writer.close_all();
    tc.speed_bps.store(0.0, std::memory_order_relaxed);
    if (!tc.segs.empty()) write_sidecar(tc);  // 有分段布局才写（否则文件已清理/不存在）

finalize_common:
    res.bytes = sum_done(tc);
    res.http_status = tc.probe.http_status;
    res.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - tc.t0).count();
    res.resumed = tc.resumed.load(std::memory_order_relaxed);
    res.range_used = tc.using_range.load(std::memory_order_relaxed);
    res.segments_used = static_cast<int32_t>(tc.segs.size());
    set_phase(tc, res.err == PJF_OK ? PJF_PHASE_DONE : PJF_PHASE_ERROR);
    {
        std::lock_guard<std::mutex> lk(tc.err_m);
        if (res.err != PJF_OK && tc.err_msg.empty()) tc.err_msg = res.message;
    }
    tc.finished.store(true, std::memory_order_release);
    tc.wait_cv.notify_all();

    // 关键：sampler 不参与分段汇合，但运行中会访问 tc。orchestrator 若直接返回、
    // 调用方随即销毁 TaskCtx，还在睡眠的 sampler 醒来即访问已释放内存（UAF）——
    // 因此必须等它真正退出。
    for (int i = 0; i < 200 && !tc.sampler_done.load(std::memory_order_acquire); ++i)
        pjb_sleep_ms(tc.engine, 5);
}

void orchestrator_thunk(void* ud) {
    auto* t = static_cast<OrchThunk*>(ud);
    orchestrator(*t->tc);
    delete t;
}

// sampler（唯一回调线程；250ms tick、EMA 速度、~2s sidecar 周期写）
void sampler(TaskCtx& tc) {
    using clock = std::chrono::steady_clock;
    auto last_tick = clock::now();
    auto last_status_emit = clock::now() - std::chrono::hours(1);  // 立即发首帧
    auto last_meta_write = clock::now() - std::chrono::hours(1);
    int32_t last_phase = PJF_PHASE_INIT;
    int64_t last_bytes = 0;
    double ema = 0.0;

    // 先发一帧再睡：下载若在 250ms 内结束也至少触发一次回调；首个采样点 dt≈0 报 0 速率。
    while (!tc.finished.load(std::memory_order_acquire)) {
        auto now = clock::now();
        double dt = std::chrono::duration<double>(now - last_tick).count();

        int64_t downloaded = sum_done(tc);
        int64_t total = tc.total_bytes.load(std::memory_order_relaxed);
        double inst = dt > 0.05 ? static_cast<double>(downloaded - last_bytes) / dt : 0.0;
        last_bytes = downloaded;
        last_tick = now;
        if (dt > 0.05) ema = ema * 0.75 + inst * 0.25;  // ~1s 时间常数
        tc.speed_bps.store(ema < 0 ? 0.0 : ema, std::memory_order_relaxed);

        if (tc.progress_cb) {
            pjf_progress p{};
            p.total_bytes = total;
            p.downloaded_bytes = downloaded;
            p.bytes_per_sec = ema < 0 ? 0.0 : ema;
            p.eta_seconds =
                (total > 0 && ema > 1.0) ? static_cast<double>(total - downloaded) / ema : -1.0;
            p.active_segments = 0;
            for (auto& s : tc.segs)
                p.active_segments += s->active.load(std::memory_order_relaxed);
            p.total_segments = static_cast<int32_t>(tc.segs.size());
            p.http_status = tc.http_status.load(std::memory_order_relaxed);
            p.using_range = tc.using_range.load(std::memory_order_relaxed);
            tc.progress_cb(&p, tc.progress_ud);
        }

        int32_t phase = tc.phase.load(std::memory_order_relaxed);
        if (phase != last_phase ||
            std::chrono::duration<double>(now - last_status_emit).count() >= 1.0) {
            if (tc.status_cb) {
                tc.status_cb(phase, tc.last_err.load(std::memory_order_relaxed),
                             tc.http_status.load(std::memory_order_relaxed), tc.status_ud);
            }
            last_phase = phase;
            last_status_emit = now;
        }

        // ~2s 周期写 sidecar（仅 DOWNLOAD 且有分段布局时）
        if (phase == PJF_PHASE_DOWNLOAD && !tc.segs.empty() &&
            std::chrono::duration<double>(now - last_meta_write).count() >= 2.0) {
            write_sidecar(tc);
            last_meta_write = now;
        }

        if (tc.finished.load(std::memory_order_acquire)) break;
        // 睡眠切 25ms 小片：保持 ~250ms 节拍，同时能及时观察到 finished
        for (int s = 0; s < 10 && !tc.finished.load(std::memory_order_acquire); ++s)
            pjb_sleep_ms(tc.engine, 25);
    }
    // 终态 status 回调（DONE/ERROR 一次性补发）
    if (tc.status_cb) {
        tc.status_cb(tc.phase.load(std::memory_order_relaxed),
                     tc.last_err.load(std::memory_order_relaxed),
                     tc.http_status.load(std::memory_order_relaxed), tc.status_ud);
    }
    // 宣告退出：orchestrator 以此为收尾同步点（之后 tc 不再被本协程触碰）
    tc.sampler_done.store(true, std::memory_order_release);
}

} // namespace

void TaskCtx::set_error(pjf_err e, const std::string& msg) {
    // 首因优先；disk_full/canceled 语义更强，允许覆盖
    pjf_err expected = PJF_OK;
    if (last_err.compare_exchange_strong(expected, e, std::memory_order_relaxed) ||
        e == PJF_ERR_DISK_FULL || e == PJF_ERR_CANCELED) {
        last_err.store(e, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(err_m);
        err_msg = msg;
    }
}

void TaskCtx::record_headers(const ProbeResult& pr) {
    std::lock_guard<std::mutex> lk(hdr_m);
    auto put = [&](const char* k, const std::string& v) {
        if (!v.empty()) headers[k] = v;
    };
    put("ETag", pr.etag);
    put("Last-Modified", pr.last_modified);
    put("Content-Type", pr.content_type);
    put("Content-Disposition", pr.content_disposition);
    put("Accept-Ranges", pr.accept_ranges);
    if (pr.total >= 0) put("Content-Length", std::to_string(pr.total));
}

TaskOptions normalize_options(const pjf_options* o, pjf_err* err, std::string* why) {
    TaskOptions t;
    auto bad = [&](pjf_err e, const char* w) {
        if (err) *err = e;
        if (why) *why = w;
        return t;
    };
    if (!o) return bad(PJF_ERR_INVALID_ARG, "options is null");
    if (!o->url || !o->url[0]) return bad(PJF_ERR_INVALID_ARG, "url is empty");
    t.url = o->url;
    std::string scheme = t.url.substr(0, 8);
    for (auto& c : scheme) c = static_cast<char>(c >= 'A' && c <= 'Z' ? char(c + 32) : c);
    if (scheme.rfind("http://", 0) != 0 && scheme.rfind("https://", 0) != 0)
        return bad(PJF_ERR_BAD_URL, "only http/https supported");
    if (o->output_path) t.output_path = o->output_path;
    if (o->user_agent) t.user_agent = o->user_agent;
    if (o->referer) t.referer = o->referer;
    if (o->headers) t.headers = o->headers;
    if (o->proxy) {
        if (o->proxy[0]) t.proxy = o->proxy;  // 具体代理地址
        else t.proxy = "<none>";              // 显式空串 = 禁代理；NULL = env 默认（留空）
    }
    t.segments = o->N_segments == 0 ? 8 : o->N_segments;
    if (t.segments < 1) t.segments = 1;
    if (t.segments > 64) t.segments = 64;
    t.max_retry = o->max_retry == 0 ? 5 : o->max_retry;
    if (t.max_retry < 0) t.max_retry = 0;
    t.connect_timeout_ms = o->connect_timeout_ms == 0 ? 15000 : o->connect_timeout_ms;
    if (t.connect_timeout_ms < 0) t.connect_timeout_ms = 0;
    t.timeout_ms = o->timeout_ms > 0 ? o->timeout_ms : 0;
    t.low_speed_limit = o->low_speed_limit == 0 ? 1024 : o->low_speed_limit;
    t.low_speed_secs = o->low_speed_secs == 0 ? 20 : o->low_speed_secs;
    t.min_segment_bytes = o->min_segment_bytes == 0 ? (1 << 20) : o->min_segment_bytes;
    if (t.min_segment_bytes < 1) t.min_segment_bytes = 1;
    t.flags = o->flags;
    t.engine_min_threads = o->engine_min_threads;
    t.engine_max_threads = o->engine_max_threads;
    if (err) *err = PJF_OK;
    return t;
}

} // namespace pjf

// C ABI（全局作用域 + extern "C"，与公共头声明严格一致）
using namespace pjf;

extern "C" PJFETCH_API pjf_downloader_t pjf_downloader_create(const pjf_options* o) {
    pjf_err e = PJF_OK;
    std::string why;
    TaskOptions opt = normalize_options(o, &e, &why);
    if (e != PJF_OK) return nullptr;

    auto* d = new (std::nothrow) DownloaderImpl();
    if (!d) return nullptr;
    d->opt = opt;

    int32_t min_t = opt.engine_min_threads > 0 ? opt.engine_min_threads : opt.segments;
    int32_t max_t = opt.engine_max_threads > 0 ? opt.engine_max_threads : 2 * opt.segments;
    if (max_t < min_t) max_t = min_t;
    d->engine = pjb_engine_create(min_t, max_t);
    if (!d->engine) {
        delete d;
        return nullptr;
    }
    return reinterpret_cast<pjf_downloader_t>(d);
}

extern "C" PJFETCH_API void pjf_downloader_destroy(pjf_downloader_t dl) {
    DownloaderImpl* d = D(dl);
    if (!d) return;
    // 等全部任务结束（契约：回调内禁止 destroy；此处可安全阻塞）
    std::vector<TaskImpl*> pending;
    {
        std::lock_guard<std::mutex> lk(d->tasks_m);
        for (auto& t : d->tasks) pending.push_back(t.get());
    }
    for (TaskImpl* ti : pending)
        pjf_download_wait(reinterpret_cast<pjf_task_t>(ti), 0);
    if (d->engine) pjb_engine_destroy(d->engine);
    delete d;
}

extern "C" PJFETCH_API pjf_task_t pjf_download_start(pjf_downloader_t dl,
                                                     const char* out_override,
                                                     pjf_progress_cb pcb, void* pcb_ud,
                                                     pjf_status_cb scb, void* scb_ud) {
    DownloaderImpl* d = D(dl);
    if (!d) return nullptr;

    auto task = std::make_unique<TaskImpl>();
    TaskCtx& tc = task->ctx;
    tc.opt = d->opt;
    if (out_override && out_override[0]) tc.opt.output_path = out_override;
    tc.out_path = derive_output_path(tc.opt);
    tc.part_path = tc.out_path + ".part";
    tc.meta_path = tc.out_path + ".pjmeta";
    tc.engine = d->engine;
    tc.cancel = std::make_shared<std::atomic_bool>(false);
    tc.progress_cb = pcb;
    tc.progress_ud = pcb_ud;
    tc.status_cb = scb;
    tc.status_ud = scb_ud;
    tc.t0 = std::chrono::steady_clock::now();

    auto* orch = new OrchThunk{&tc};
    if (pjb_submit(d->engine, orchestrator_thunk, orch, nullptr, nullptr) ==
        PJB_INVALID_JOB) {
        delete orch;
        return nullptr;
    }
    TaskImpl* raw = task.get();
    {
        std::lock_guard<std::mutex> lk(d->tasks_m);
        d->tasks.push_back(std::move(task));
    }
    return reinterpret_cast<pjf_task_t>(raw);
}

extern "C" PJFETCH_API pjf_err pjf_download_wait(pjf_task_t t, uint32_t timeout_ms) {
    TaskImpl* ti = T(t);
    if (!ti) return PJF_ERR_INVALID_ARG;
    std::unique_lock<std::mutex> lk(ti->ctx.wait_m);
    if (timeout_ms == 0) {
        ti->ctx.wait_cv.wait(
            lk, [&] { return ti->ctx.finished.load(std::memory_order_acquire); });
        return PJF_OK;
    }
    bool done = ti->ctx.wait_cv.wait_for(
        lk, std::chrono::milliseconds(timeout_ms),
        [&] { return ti->ctx.finished.load(std::memory_order_acquire); });
    return done ? PJF_OK : PJF_ERR_TIMEOUT;
}

extern "C" PJFETCH_API void pjf_download_cancel(pjf_task_t t) {
    TaskImpl* ti = T(t);
    if (!ti) return;
    TaskCtx& tc = ti->ctx;
    tc.canceled.store(true, std::memory_order_relaxed);
    if (tc.cancel) tc.cancel->store(true);  // 唤醒所有 in-flight cpr 会话
}

extern "C" PJFETCH_API int32_t pjf_download_is_running(pjf_task_t t) {
    TaskImpl* ti = T(t);
    if (!ti) return -1;
    return ti->ctx.finished.load(std::memory_order_acquire) ? 0 : 1;
}

extern "C" PJFETCH_API pjf_err pjf_download_result(pjf_task_t t, pjf_result* out) {
    TaskImpl* ti = T(t);
    if (!ti || !out) return PJF_ERR_INVALID_ARG;
    if (!ti->ctx.finished.load(std::memory_order_acquire))
        return PJF_ERR_INTERNAL;  // wait 成功后有效（契约）
    *out = ti->ctx.result;
    return PJF_OK;
}

extern "C" PJFETCH_API pjf_err pjf_query_progress(pjf_task_t t, pjf_progress* out) {
    TaskImpl* ti = T(t);
    if (!ti || !out) return PJF_ERR_INVALID_ARG;
    TaskCtx& tc = ti->ctx;
    out->total_bytes = tc.total_bytes.load(std::memory_order_relaxed);
    out->downloaded_bytes = sum_done(tc);
    out->bytes_per_sec = tc.speed_bps.load(std::memory_order_relaxed);
    int64_t total = out->total_bytes;
    out->eta_seconds = (total > 0 && out->bytes_per_sec > 1.0)
                           ? static_cast<double>(total - out->downloaded_bytes) /
                                 out->bytes_per_sec
                           : -1.0;
    out->active_segments = 0;
    for (auto& s : tc.segs) out->active_segments += s->active.load(std::memory_order_relaxed);
    out->total_segments = static_cast<int32_t>(tc.segs.size());
    out->http_status = tc.http_status.load(std::memory_order_relaxed);
    out->using_range = tc.using_range.load(std::memory_order_relaxed);
    return PJF_OK;
}

extern "C" PJFETCH_API int32_t pjf_query_segments(pjf_task_t t, pjf_segment* buf, int32_t cap) {
    TaskImpl* ti = T(t);
    if (!ti) return -1;
    TaskCtx& tc = ti->ctx;
    if (!buf || cap <= 0) return static_cast<int32_t>(tc.segs.size());
    int32_t n = 0;
    for (size_t i = 0; i < tc.segs.size() && n < cap; ++i, ++n) {
        auto& s = tc.segs[i];
        buf[n].index = static_cast<int32_t>(i);
        buf[n].start = s->start;
        buf[n].end = s->end;
        buf[n].done = s->done.load(std::memory_order_relaxed);
        buf[n].attempts = s->attempts.load(std::memory_order_relaxed);
        buf[n].finished = s->finished.load(std::memory_order_relaxed);
    }
    return n;
}

extern "C" PJFETCH_API int32_t pjf_get_header(pjf_task_t t, const char* name, char* buf,
                                              int32_t cap) {
    TaskImpl* ti = T(t);
    if (!ti || !name || !buf || cap <= 0) return -1;
    TaskCtx& tc = ti->ctx;
    std::lock_guard<std::mutex> lk(tc.hdr_m);
    auto it = tc.headers.find(name);
    if (it == tc.headers.end()) return -1;
    int32_t len = static_cast<int32_t>(it->second.size());
    int32_t copy = len < cap - 1 ? len : cap - 1;
    std::memcpy(buf, it->second.data(), static_cast<size_t>(copy));
    buf[copy] = '\0';
    return len;
}

extern "C" PJFETCH_API int32_t pjf_last_error(pjf_task_t t, char* buf, int32_t cap) {
    TaskImpl* ti = T(t);
    if (!ti || !buf || cap <= 0) return -1;
    TaskCtx& tc = ti->ctx;
    std::string msg;
    {
        std::lock_guard<std::mutex> lk(tc.err_m);
        msg = tc.err_msg.empty() ? pjf_err_str(tc.last_err.load(std::memory_order_relaxed))
                                 : tc.err_msg;
    }
    int32_t len = static_cast<int32_t>(msg.size());
    int32_t copy = len < cap - 1 ? len : cap - 1;
    std::memcpy(buf, msg.data(), static_cast<size_t>(copy));
    buf[copy] = '\0';
    return len;
}

extern "C" PJFETCH_API pjf_err pjf_download(pjf_downloader_t dl, const pjf_options* opts,
                                            pjf_progress_cb pcb, void* ud, pjf_result* out) {
    DownloaderImpl* d = D(dl);
    if (!d || !out) return PJF_ERR_INVALID_ARG;
    // 便捷语义：沿用创建时的选项；opts 非 NULL 时 url 必须一致（output_path 可覆盖本次）
    if (opts && opts->url && opts->url[0] && opts->url != d->opt.url)
        return PJF_ERR_INVALID_ARG;
    pjf_task_t t = pjf_download_start(
        dl, (opts && opts->output_path && opts->output_path[0]) ? opts->output_path : nullptr,
        pcb, ud, nullptr, nullptr);
    if (!t) return PJF_ERR_INTERNAL;
    pjf_err w = pjf_download_wait(t, 0);
    if (w != PJF_OK) return w;
    return pjf_download_result(t, out);
}

extern "C" PJFETCH_API const char* pjf_version(void) {
    return "0.1.0";
}
