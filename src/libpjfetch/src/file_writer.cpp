// 文件写入接缝：每段一个独立 ofstream 实例打开同一 .part 路径（MSVC/POSIX 均允许同进程
// 多句柄读写同一文件）。逐 chunk flush，保证被 kill 后 .part 已写部分与 .pjmeta 记录一致。
#include "downloader_impl.hpp"

#include <cstdio>
#include <fstream>

namespace pjf {

FileWriter::~FileWriter() {
    close_all();
}

bool FileWriter::open_part(const std::string& path, int64_t total) {
    close_all();
    path_ = path;
    part_size_ = 0;
    {
        std::ofstream f(path_, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!f) {
            disk_full_ = true;
            return false;
        }
    }
    if (total > 0) {
        // 预分配：seekp(size-1) + 写 1 字节
        std::ofstream f(path_, std::ios::binary | std::ios::in | std::ios::out);
        if (!f) {
            disk_full_ = true;
            return false;
        }
        f.seekp(total - 1);
        f.put('\0');
        f.flush();
        if (!f) {
            disk_full_ = true;
            return false;
        }
        part_size_ = total;
    }
    return true;
}

bool FileWriter::open_part_keep(const std::string& path) {
    close_all();
    path_ = path;
    // 仅探测可写性：以追加模式打开一次（不修改内容）
    std::ofstream f(path_, std::ios::binary | std::ios::app);
    if (!f) {
        disk_full_ = true;
        return false;
    }
    f.seekp(0, std::ios::end);
    part_size_ = static_cast<int64_t>(f.tellp());
    return true;
}

void FileWriter::reserve_segments(size_t n) {
    // 编排器单线程调用：一次性定容，之后 open_segment 不再 realloc（并发 resize 会堆损坏）。
    if (segs_.size() < n) segs_.resize(n);
}

bool FileWriter::open_segment(size_t i, int64_t offset) {
    if (segs_.size() <= i) segs_.resize(i + 1);   // 兜底；正常路径已被 reserve_segments 定容
    if (!segs_[i]) segs_[i] = std::make_unique<Seg>();
    Seg& seg = *segs_[i];
    if (!seg.open) {
        seg.stream.open(path_, std::ios::binary | std::ios::in | std::ios::out);
        if (!seg.stream) {
            disk_full_ = true;
            return false;
        }
        seg.open = true;
    }
    // 每次 attempt 重新定位到 start+done；clear() 复位上次中止留下的 fail/bad 状态
    seg.stream.clear();
    seg.stream.seekp(offset);
    if (!seg.stream) {
        disk_full_ = true;
        return false;
    }
    return true;
}

bool FileWriter::write(size_t i, const char* data, size_t len) {
    if (i >= segs_.size() || !segs_[i] || !segs_[i]->open) {
        disk_full_ = true;
        return false;
    }
    std::ofstream& s = segs_[i]->stream;
    if (len > 0) s.write(data, static_cast<std::streamsize>(len));
    s.flush();
    if (!s) {
        disk_full_ = true;
        return false;
    }
    return true;
}

bool FileWriter::restart_stream(const std::string& path) {
    // 200-collapse：Range 被服务器忽略，截断 .part 重开为单流
    close_all();
    path_ = path;
    part_size_ = 0;
    segs_.clear();
    segs_.push_back(std::make_unique<Seg>());
    segs_[0]->stream.open(path_, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!segs_[0]->stream) {
        disk_full_ = true;
        return false;
    }
    segs_[0]->open = true;
    return true;
}

void FileWriter::close_all() {
    for (auto& seg : segs_) {
        if (seg && seg->open) {
            seg->stream.flush();
            seg->stream.close();
            seg->open = false;
        }
    }
}

} // namespace pjf
