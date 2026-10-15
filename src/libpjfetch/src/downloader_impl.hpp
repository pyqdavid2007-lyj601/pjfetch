#ifndef PJFETCH_DOWNLOADER_IMPL_HPP
#define PJFETCH_DOWNLOADER_IMPL_HPP

// libpjfetch 内部实现头（绝不进公共 ABI）。pjf_downloader 持有若干 pjf_task；
// 每个 task = 1 编排器 job + N 分段 job + 1 sampler job，全部跑在 pjb_engine 上，
// 用户回调只从 sampler job 触发。跨 job 状态只用 atomic / mutex / cv。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "parajobs/parajobs.h"
#include "pjfetch/pjfetch.h"

// 前置声明（全局作用域——放 pjf 内会遮蔽真实 ::cpr 命名空间）
namespace cpr {
class Session;
}

namespace pjf {

// ASCII 大小写不敏感比较（响应头查找用）
inline int ci_compare(const std::string& a, const std::string& b) {
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        int la = (ca >= 'A' && ca <= 'Z') ? ca + 32 : ca;
        int lb = (cb >= 'A' && cb <= 'Z') ? cb + 32 : cb;
        if (la != lb) return la < lb ? -1 : 1;
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

struct TaskOptions {
    std::string url;
    std::string output_path;          // 空 => 由 URL 推导
    int32_t segments = 8;             // 1..64
    int32_t max_retry = 5;
    int32_t connect_timeout_ms = 15000;
    int32_t timeout_ms = 0;           // 0 = 不限
    int32_t low_speed_limit = 1024;
    int32_t low_speed_secs = 20;
    int64_t min_segment_bytes = 1 << 20;
    std::string user_agent;
    std::string referer;
    std::string proxy;                // 空 = env 默认；"<none>" 内部表示禁代理
    std::string headers;
    uint32_t flags = 0;
    int32_t engine_min_threads = 0;
    int32_t engine_max_threads = 0;
};

TaskOptions normalize_options(const pjf_options* o, pjf_err* err, std::string* why);

// 分段运行态（跨 job 共享，全部原子）
struct SegmentState {
    int64_t start = 0;
    int64_t end = -1;                 // 计划区间终点（含）；-1 = 未知大小单流
    std::atomic<int64_t> done{0};
    std::atomic<int32_t> attempts{0};
    std::atomic<int32_t> finished{0};
    std::atomic<int32_t> active{0};   // 正处于一次 cpr perform 中

    int64_t span() const { return end < 0 ? -1 : end - start + 1; }
};

// 文件写入接缝（file_writer.cpp）——每段一个 ofstream
class FileWriter {
public:
    ~FileWriter();

    // 打开/预分配 .part（total<0 = 未知大小，不预分配）
    bool open_part(const std::string& path, int64_t total);
    // 断点续传：保留既有 .part（不截断/不预分配）
    bool open_part_keep(const std::string& path);
    // 段槽位必须由编排器在任何分段任务启动前单线程定容一次：
    // 多个分段协程并发 resize 同一个 vector 会造成堆损坏。
    void reserve_segments(size_t n);
    bool open_segment(size_t i, int64_t offset);
    // 写失败置 disk_full
    bool write(size_t i, const char* data, size_t len);
    bool restart_stream(const std::string& path);
    void close_all();
    bool disk_full() const { return disk_full_; }
    int64_t part_size_hint() const { return part_size_; }

private:
    struct Seg {
        std::ofstream stream;         // 每段独立流实例，同一 .part 路径
        bool open = false;
    };
    std::vector<std::unique_ptr<Seg>> segs_;
    std::string path_;
    int64_t part_size_ = 0;
    std::atomic<bool> disk_full_{false};
};

struct MetaData {
    int version = 1;
    std::string url;
    std::string etag;
    std::string last_modified;
    int64_t total = -1;
    int segments = 0;
    int range_mode = 0;
    std::vector<int64_t> seg_done;
    std::vector<int32_t> seg_finished;
};

bool meta_save(const std::string& path, const MetaData& m);
std::optional<MetaData> meta_load(const std::string& path);
// 恢复准入校验（url/total/.part 大小/etag|last_modified/sum(seg.done)）
bool meta_validate_resume(const MetaData& m, const std::string& url, int64_t part_size,
                          const std::string& etag, const std::string& last_modified,
                          std::string* why_not);

struct ProbeResult {
    bool ok = false;
    pjf_err err = PJF_OK;
    std::string message;
    int64_t total = -1;               // -1 = 未知
    bool range_ok = false;            // 诚实检测：亲眼见过 206
    int http_status = 0;
    std::string etag;
    std::string last_modified;
    std::string content_type;
    std::string content_disposition;
    std::string accept_ranges;        // 仅记录，不作为依据
    std::string final_url;            // 重定向后的 URL（resume 校验用）
};

struct TaskCtx;

ProbeResult run_probe(TaskCtx& tc);

// 规划分段：返回段区间；单流条件满足时恰 1 段（end=-1 或全长）
std::vector<std::pair<int64_t, int64_t>> plan_segments(int64_t total, bool range_ok,
                                                       const TaskOptions& opt);

// cpr 会话工厂（session_factory.cpp）——库内唯一创建 cpr::Session 的地方，cpr 头绝不进本头
struct SessionDeleter {
    void operator()(cpr::Session* s) const;
};
using SessionPtr = std::unique_ptr<cpr::Session, SessionDeleter>;
SessionPtr make_session(const TaskOptions& opt, const std::string& url,
                        std::shared_ptr<std::atomic_bool> cancel);

pjf_err map_cpr_error(int cpr_code);           // cpr::ErrorCode 强转 int
bool http_retryable(int status);               // 429 / 5xx
pjf_err classify_http(int status);             // 4xx(非 429) → HTTP_STATUS
const char* pjf_err_str(pjf_err e);

void run_segment_job(TaskCtx& tc, size_t seg_index);

struct TaskCtx {
    TaskOptions opt;
    std::string out_path;
    std::string part_path;            // <out>.part
    std::string meta_path;            // <out>.pjmeta
    std::shared_ptr<std::atomic_bool> cancel;  // cpr SetCancellationParam 共享
    pjb_engine_t engine = nullptr;

    // 探测结果（编排器写；sampler/外部线程读——由 phase 序保证）
    ProbeResult probe;

    // plan 后固定大小；collapse 后重置为 1 段
    std::vector<std::unique_ptr<SegmentState>> segs;

    std::atomic<bool> sampler_done{false};    // sampler 协程已退出（收尾必须等它，防 UAF）
    std::atomic<bool> collapse{false};        // 200-collapse：Range 被服务器忽略
    std::atomic<bool> canceled{false};
    std::atomic<bool> disk_full{false};
    std::atomic<bool> fatal{false};           // 任一段致命错误 => 全体退出
    std::atomic<pjf_err> last_err{PJF_OK};
    std::atomic<int> http_status{0};
    std::atomic<int32_t> phase{PJF_PHASE_INIT};
    std::atomic<int64_t> total_bytes{-1};
    std::atomic<int> using_range{0};
    std::atomic<int> resumed{0};
    std::atomic<int> active_jobs{0};          // 未退出的分段 job 数
    std::atomic<bool> finished{false};

    std::mutex err_m;
    std::string err_msg;
    void set_error(pjf_err e, const std::string& msg);

    std::mutex hdr_m;
    std::map<std::string, std::string, int (*)(const std::string&, const std::string&)> headers{
        [](const std::string& a, const std::string& b) { return ci_compare(a, b); }};

    FileWriter writer;
    std::mutex meta_m;                // 串行化 sampler 周期写与编排器终写
    std::chrono::steady_clock::time_point t0;

    std::mutex wait_m;
    std::condition_variable wait_cv;

    // 用户回调（仅 sampler job 调用）
    pjf_progress_cb progress_cb = nullptr;
    void* progress_ud = nullptr;
    pjf_status_cb status_cb = nullptr;
    void* status_ud = nullptr;
    std::atomic<double> speed_bps{0.0};  // sampler 维护的 EMA（query_progress 读）

    // 结果（finished 后只读）
    pjf_result result{};

    void record_headers(const struct ProbeResult& pr);
};

} // namespace pjf

#endif // PJFETCH_DOWNLOADER_IMPL_HPP
