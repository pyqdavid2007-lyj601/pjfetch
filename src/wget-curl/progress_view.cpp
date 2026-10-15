#include "progress_view.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "console.hpp"

namespace pjf_cli {

std::string human_bytes(double bytes) {
    if (bytes < 0 || !std::isfinite(bytes)) return "?";
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double v = bytes;
    int u = 0;
    while (v >= 1024.0 && u < 5) { v /= 1024.0; ++u; }
    char buf[64];
    if (u == 0)
        std::snprintf(buf, sizeof buf, "%.0f%s", v, units[u]);
    else
        std::snprintf(buf, sizeof buf, "%.1f%s", v, units[u]);
    return buf;
}

std::string format_eta(double seconds) {
    if (seconds < 0 || !std::isfinite(seconds)) return "--:--";
    long long s = (long long)(seconds + 0.5);
    long long h = s / 3600;
    int m = int((s % 3600) / 60), sec = int(s % 60);
    char buf[32];
    if (h > 0)
        std::snprintf(buf, sizeof buf, "%lld:%02d:%02d", h, m, sec);
    else
        std::snprintf(buf, sizeof buf, "%02d:%02d", m, sec);
    return buf;
}

std::string format_progress_line(const pjf_progress& p, const std::string& name) {
    char buf[128];
    std::string line = name;
    bool known = p.total_bytes > 0;
    if (known) {
        double frac = double(p.downloaded_bytes) / double(p.total_bytes);
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        std::snprintf(buf, sizeof buf, " %.1f%% ", frac * 100.0);
        line += buf;
        const int bar_w = 20;
        int fill = int(frac * bar_w + 1e-9);
        line += '[';
        if (fill >= bar_w) {
            line.append(size_t(bar_w), '=');
        } else {
            line.append(size_t(fill), '=');
            line += '>';
            line.append(size_t(bar_w - fill - 1), ' ');
        }
        line += "] ";
        line += human_bytes(double(p.downloaded_bytes));
        line += '/';
        line += human_bytes(double(p.total_bytes));
    } else {
        line += ' ';
        line += human_bytes(double(p.downloaded_bytes));
    }
    if (p.bytes_per_sec > 0) {
        line += " @ ";
        line += human_bytes(p.bytes_per_sec);
        line += "/s";
    }
    if (known && p.eta_seconds >= 0) {
        line += " ETA ";
        line += format_eta(p.eta_seconds);
    }
    if (!known) {
        line += " streaming";
    }
    if (p.total_segments > 0) {
        std::snprintf(buf, sizeof buf, " (%d seg)", p.total_segments);
        line += buf;
    }
    return line;
}

progress_view::progress_view(bool enabled)
    : enabled_(enabled), last_print_(std::chrono::steady_clock::time_point::min()) {}

void progress_view::update(const pjf_progress& p, const std::string& name) {
    if (!enabled_) return;
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_print_).count() < 250)
        return;
    last_print_ = now;

    std::string line = format_progress_line(p, name);
    int width = console_width();
    if (width > 1 && line.size() > size_t(width - 1)) line.resize(size_t(width - 1));
    if (line.size() < last_len_) line.append(last_len_ - line.size(), ' ');
    last_len_ = line.size();
    std::fwrite("\r", 1, 1, stderr);
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
}

void progress_view::clear() {
    if (!enabled_ || last_len_ == 0) return;
    std::string blank(last_len_ + 1, ' ');
    std::fwrite("\r", 1, 1, stderr);
    std::fwrite(blank.data(), 1, blank.size(), stderr);
    std::fwrite("\r", 1, 1, stderr);
    std::fflush(stderr);
    last_len_ = 0;
}

} // namespace pjf_cli
