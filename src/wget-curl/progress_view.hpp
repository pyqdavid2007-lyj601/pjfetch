#ifndef WGET_CURL_PROGRESS_VIEW_HPP
#define WGET_CURL_PROGRESS_VIEW_HPP

// 单行进度视图（stderr，经 console 隔离层）：250ms 节流，按 console_width() 截断并在尾部
// 补空格抹除残影，结束时 clear() 清行。格式（未知大小时省去百分比/进度条/ETA 并加 streaming）：
//   `name 42.3% [====>        ] 512.4MiB/1.18GiB @ 8.5MiB/s ETA 01:12 (8 seg)`

#include <chrono>
#include <string>

#include "pjfetch/pjfetch.h"

namespace pjf_cli {

std::string human_bytes(double bytes);            // 1024 基：512.4MiB（无空格）
std::string format_eta(double seconds);           // mm:ss / h:mm:ss；未知 -> "--:--"
std::string format_progress_line(const pjf_progress& p, const std::string& name);

class progress_view {
public:
    explicit progress_view(bool enabled);
    // disabled 时为空操作
    void update(const pjf_progress& p, const std::string& name);
    void clear();

private:
    bool enabled_;
    size_t last_len_ = 0;
    std::chrono::steady_clock::time_point last_print_;
};

} // namespace pjf_cli

#endif // WGET_CURL_PROGRESS_VIEW_HPP
