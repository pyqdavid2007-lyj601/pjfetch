#include "downloader_impl.hpp"

namespace pjf {

std::vector<std::pair<int64_t, int64_t>> plan_segments(int64_t total, bool range_ok,
                                                       const TaskOptions& opt) {
    std::vector<std::pair<int64_t, int64_t>> segs;
    int64_t min_seg = opt.min_segment_bytes > 0 ? opt.min_segment_bytes : (1 << 20);

    // 单流条件：无 Range / 大小未知 / 文件 < 2*min_segment_bytes / 用户钳为 1
    if (!range_ok || total < 0 || total < 2 * min_seg || opt.segments <= 1) {
        // 单段。end 语义双重：既表示区间终点，也是"是否发 Range 头"的开关（end >= 0 才发）。
        // 不支持 Range 或大小未知时必须填 -1——即使 total 已知也不能填 total-1，否则会给该服务器发 Range 头。
        segs.emplace_back(int64_t{0}, range_ok && total >= 0 ? total - 1 : int64_t{-1});
        return segs;
    }

    int n = opt.segments;
    if (n > 64) n = 64;
    if (n < 1) n = 1;
    if (total / n < min_seg) {
        n = static_cast<int>(total / min_seg);
        if (n < 1) n = 1;
    }
    if (n == 1) {
        segs.emplace_back(int64_t{0}, total - 1);
        return segs;
    }

    int64_t base = total / n;
    int64_t rem = total % n;
    int64_t pos = 0;
    for (int i = 0; i < n; ++i) {
        int64_t len = base + (i < rem ? 1 : 0);
        segs.emplace_back(pos, pos + len - 1);
        pos += len;
    }
    return segs;
}

} // namespace pjf
