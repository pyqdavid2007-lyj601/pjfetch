// <out>.pjmeta sidecar：ASCII key=value，.tmp + 原子替换写；恢复准入全规则校验。
#include "downloader_impl.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace pjf {

namespace {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// 值中不允许换行（保证一行一 key=value 的可解析性）
std::string sanitize(const std::string& s) {
    std::string r = s;
    for (auto& c : r)
        if (c == '\r' || c == '\n') c = ' ';
    return r;
}

void write_kv(std::ostream& os, const char* k, const std::string& v) {
    os << k << '=' << sanitize(v) << '\n';
}

} // namespace

bool meta_save(const std::string& path, const MetaData& m) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream os(tmp, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!os) return false;
        write_kv(os, "version", std::to_string(m.version));
        write_kv(os, "url", m.url);
        write_kv(os, "etag", m.etag);
        write_kv(os, "last_modified", m.last_modified);
        write_kv(os, "total", std::to_string(m.total));
        write_kv(os, "segments", std::to_string(m.segments));
        write_kv(os, "range_mode", std::to_string(m.range_mode));
        for (int i = 0; i < m.segments && i < static_cast<int>(m.seg_done.size()); ++i) {
            write_kv(os, ("seg" + std::to_string(i) + ".done").c_str(),
                     std::to_string(m.seg_done[static_cast<size_t>(i)]));
            int fin = i < static_cast<int>(m.seg_finished.size()) ? m.seg_finished[static_cast<size_t>(i)] : 0;
            write_kv(os, ("seg" + std::to_string(i) + ".finished").c_str(),
                     std::to_string(fin));
        }
        os.flush();
        if (!os) return false;
    }
    // 原子替换：rename 在 Windows/POSIX 均原子覆盖，无"旧已删新未落"窗口
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        // 兜底：Windows 上目标被独占打开时 rename 可能失败
        std::remove(path.c_str());
        std::error_code ec2;
        std::filesystem::rename(tmp, path, ec2);
        if (ec2) return false;
    }
    return true;
}

std::optional<MetaData> meta_load(const std::string& path) {
    std::ifstream is(path, std::ios::binary);
    if (!is) return std::nullopt;
    MetaData m;
    m.seg_done.reserve(64);
    m.seg_finished.reserve(64);
    std::string line;
    while (std::getline(is, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq));
        std::string v = trim(line.substr(eq + 1));
        auto to_i64 = [](const std::string& s, int64_t dflt) -> int64_t {
            try { return std::stoll(s); } catch (...) { return dflt; }
        };
        if (k == "version") m.version = static_cast<int>(to_i64(v, 1));
        else if (k == "url") m.url = v;
        else if (k == "etag") m.etag = v;
        else if (k == "last_modified") m.last_modified = v;
        else if (k == "total") m.total = to_i64(v, -1);
        else if (k == "segments") m.segments = static_cast<int>(to_i64(v, 0));
        else if (k == "range_mode") m.range_mode = static_cast<int>(to_i64(v, 0));
        else if (k.size() > 5 && k.compare(0, 3, "seg") == 0 && k[2] == 'g') {
            size_t dot = k.rfind('.');
            if (dot == std::string::npos) continue;
            int idx = 0;
            try { idx = std::stoi(k.substr(3, dot - 3)); } catch (...) { continue; }
            std::string field = k.substr(dot + 1);
            if (field == "done") {
                if (static_cast<int>(m.seg_done.size()) <= idx) m.seg_done.resize(static_cast<size_t>(idx) + 1, 0);
                m.seg_done[static_cast<size_t>(idx)] = to_i64(v, 0);
            } else if (field == "finished") {
                if (static_cast<int>(m.seg_finished.size()) <= idx)
                    m.seg_finished.resize(static_cast<size_t>(idx) + 1, 0);
                m.seg_finished[static_cast<size_t>(idx)] = static_cast<int>(to_i64(v, 0));
            }
        }
    }
    if (m.version != 1 || m.url.empty() || m.segments <= 0) return std::nullopt;
    if (static_cast<int>(m.seg_done.size()) < m.segments)
        m.seg_done.resize(static_cast<size_t>(m.segments), 0);
    if (static_cast<int>(m.seg_finished.size()) < m.segments)
        m.seg_finished.resize(static_cast<size_t>(m.segments), 0);
    return m;
}

bool meta_validate_resume(const MetaData& m, const std::string& url, int64_t part_size,
                          const std::string& etag, const std::string& last_modified,
                          std::string* why_not) {
    auto fail = [&](const char* why) {
        if (why_not) *why_not = why;
        return false;
    };
    if (m.url != url) return fail("url mismatch");
    if (m.total < 0) return fail("no total size recorded");
    if (part_size < m.total) return fail(".part smaller than recorded total");
    if (!m.etag.empty() || !etag.empty()) {
        if (m.etag != etag) return fail("etag mismatch");
    } else if (!m.last_modified.empty() || !last_modified.empty()) {
        if (m.last_modified != last_modified) return fail("last_modified mismatch");
    }
    int64_t sum = 0;
    for (auto d : m.seg_done) sum += d;
    if (sum > part_size) return fail("recorded progress exceeds .part size");
    return true;
}

} // namespace pjf
