// wget-curl.exe —— 基于 libpjfetch 的命令行下载工具。
// 退出码：0 成功；1 网络/HTTP；2 用法；3 本地 IO/磁盘满；4 不支持/BAD_URL；5 用户取消。

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <ctime>
#include <string>
#include <vector>

#include "pjfetch/pjfetch.h"

#include "cli.hpp"
#include "config.hpp"
#include "console.hpp"
#include "filename.hpp"
#include "progress_view.hpp"

namespace {

namespace fs = std::filesystem;
using pjf_cli::options;

std::string iso_utc_now() {
    std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm* tmv = std::gmtime(&t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", tmv);
    return buf;
}

const char* err_name(pjf_err e) {
    switch (e) {
        case PJF_OK: return "OK";
        case PJF_ERR_INVALID_ARG: return "INVALID_ARG";
        case PJF_ERR_BAD_URL: return "BAD_URL";
        case PJF_ERR_IO: return "IO";
        case PJF_ERR_DISK_FULL: return "DISK_FULL";
        case PJF_ERR_NETWORK: return "NETWORK";
        case PJF_ERR_HTTP_STATUS: return "HTTP_STATUS";
        case PJF_ERR_RANGE_UNSUPPORTED: return "RANGE_UNSUPPORTED";
        case PJF_ERR_CANCELED: return "CANCELED";
        case PJF_ERR_TOO_MANY_RETRIES: return "TOO_MANY_RETRIES";
        case PJF_ERR_TIMEOUT: return "TIMEOUT";
        case PJF_ERR_INTERNAL: return "INTERNAL";
    }
    return "?";
}

int exit_code_for(pjf_err e) {
    switch (e) {
        case PJF_OK: return 0;
        case PJF_ERR_CANCELED: return 5;
        case PJF_ERR_IO:
        case PJF_ERR_DISK_FULL: return 3;
        case PJF_ERR_BAD_URL:
        case PJF_ERR_RANGE_UNSUPPORTED: return 4;
        case PJF_ERR_INVALID_ARG: return 2;
        default: return 1;  // NETWORK / HTTP_STATUS / TOO_MANY_RETRIES / TIMEOUT / INTERNAL
    }
}

void die_usage(const std::string& msg) {
    std::fprintf(stderr, "wget-curl: %s\n", msg.c_str());
    std::fprintf(stderr, "Try 'wget-curl --help' for more information.\n");
    std::exit(2);
}

// 会话历史 ~/.pjfetch/pj.ses，每次下载起止各追加一行：
//   <ISO-UTC> START url=<url> out=<path> segments=<N>
//   <ISO-UTC> END status=<ok|canceled|failed:<NAME>> url=<url> out=<path> bytes=<n>
void ses_append(const char* line) {
    fs::path dir = pjf_cli::app_dir();
    if (dir.empty()) return;
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream out(dir / "pj.ses", std::ios::app);
    if (out) out << line << "\n";
}

// 控制文件 <out>.pjfetch.dld：CLI 层选项快照，成功即删，-c 时读取校验 URL
// （格式见下方 dld_write：ASCII key=value）。库自管的 <out>.part + <out>.pjmeta
// 是协议层状态，与本文件互不冲突。
struct dld_info {
    std::string url, output;
    int segments = 8;
    int resume = 1;
    int retry = 5;
};

fs::path dld_path(const fs::path& out) { return fs::path(out.string() + ".pjfetch.dld"); }

bool dld_write(const fs::path& out, const dld_info& d) {
    std::ofstream f(dld_path(out));
    if (!f) return false;
    f << "# pjfetch control file v1\n"
      << "version=1\n"
      << "url=" << d.url << "\n"
      << "output=" << d.output << "\n"
      << "segments=" << d.segments << "\n"
      << "resume=" << d.resume << "\n"
      << "retry=" << d.retry << "\n"
      << "created=" << iso_utc_now() << "\n";
    return bool(f);
}

// URL 不一致时告警并返回 false（调用方将覆写）。
bool dld_check_on_resume(const fs::path& out, const dld_info& cur) {
    std::ifstream f(dld_path(out));
    if (!f) return false;
    std::string line, stored_url;
    int version = 0;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "version") version = std::atoi(v.c_str());
        else if (k == "url") stored_url = v;
    }
    if (version != 1) return false;
    if (!stored_url.empty() && stored_url != cur.url) {
        std::fprintf(stderr, "wget-curl: warning: %s belongs to a different URL, ignoring saved state\n",
                     dld_path(out).string().c_str());
        return false;
    }
    return true;
}

void dld_remove(const fs::path& out) {
    std::error_code ec;
    fs::remove(dld_path(out), ec);
}

// 进度走 pjf_query_progress 轮询；回调仅用于规避契约对 NULL 的未定义疑点
void on_progress(const pjf_progress*, void*) {}
void on_status(int32_t, pjf_err, int32_t, void*) {}

void ses_append_end(const char* status, const std::string& url, const std::string& out_path,
                    long long bytes) {
    char line[1024];
    std::snprintf(line, sizeof line, "%s END status=%s url=%s out=%s bytes=%lld",
                  iso_utc_now().c_str(), status, url.c_str(), out_path.c_str(), bytes);
    ses_append(line);
}

} // namespace

int main(int argc, char** argv) {
    pjf_cli::console_init();

    options defaults;
    auto cfg = pjf_cli::load_config();
    int unknown_cfg = pjf_cli::apply_config_defaults(cfg, defaults);
    if (unknown_cfg > 0)
        std::fprintf(stderr, "wget-curl: warning: %d unknown/invalid key(s) in ~/.pjfetch/pj-cfg.cnf\n", unknown_cfg);

    options opts;
    std::string err, text;
    switch (pjf_cli::parse(pjf_cli::console_command_line(argc, argv), defaults, opts, err, text)) {
        case pjf_cli::parse_status::error:   die_usage(err);
        case pjf_cli::parse_status::help:
        case pjf_cli::parse_status::version: std::fputs(text.c_str(), stdout); return 0;
        case pjf_cli::parse_status::ok: break;
    }

    bool have_o = !opts.output.empty();
    fs::path out;
    if (have_o) {
        out = fs::path(opts.output);
    } else {
        std::string base = pjf_cli::filename_from_url(opts.url);
        if (base.empty()) base = "download.bin";
        out = opts.dir.empty() ? fs::path(base) : fs::path(opts.dir) / base;
    }
    std::string leaf = out.filename().string();

    std::error_code ec;
    if (out.has_parent_path()) {
        fs::create_directories(out.parent_path(), ec);
        if (ec) {
            std::fprintf(stderr, "wget-curl: cannot create directory '%s': %s\n",
                         out.parent_path().string().c_str(), ec.message().c_str());
            return 3;
        }
    }
    if (fs::is_directory(out, ec)) {
        std::fprintf(stderr, "wget-curl: output '%s' is a directory\n", out.string().c_str());
        return 3;
    }

    dld_info d;
    d.url = opts.url; d.output = out.string();
    d.segments = opts.para_jobs; d.resume = opts.resume ? 1 : 0; d.retry = opts.retry;
    if (opts.resume) (void)dld_check_on_resume(out, d);
    dld_write(out, d);

    {
        char line[1024];
        std::snprintf(line, sizeof line, "%s START url=%s out=%s segments=%d",
                      iso_utc_now().c_str(), opts.url.c_str(), out.string().c_str(), opts.para_jobs);
        ses_append(line);
    }

    if (!opts.quiet)
        // 尚未探测：实际段数可能更少（小文件/无 Range 时 1 段），故只说"最多 N 段"
        std::printf("downloading %s\n  -> %s (up to %d segment%s)\n", opts.url.c_str(), out.string().c_str(),
                    opts.para_jobs, opts.para_jobs == 1 ? "" : "s");

    // 静态存储保证字符串生命周期覆盖整个下载（契约只存指针）
    static std::string s_url = opts.url;
    static std::string s_out = out.string();
    static std::string s_ua = opts.user_agent, s_ref = opts.referer, s_proxy = opts.proxy,
                       s_headers = opts.headers;
    pjf_options po{};
    po.url = s_url.c_str();
    po.output_path = s_out.c_str();
    po.N_segments = opts.para_jobs;
    po.max_retry = opts.retry > 0 ? opts.retry : 0;
    po.connect_timeout_ms = int32_t(opts.connect_timeout > 0 ? opts.connect_timeout * 1000 : 0);
    po.timeout_ms = int32_t(opts.timeout > 0 ? opts.timeout * 1000 : 0);
    po.low_speed_limit = int32_t(opts.low_speed_limit > 0 ? opts.low_speed_limit : 0);
    po.low_speed_secs = int32_t(opts.low_speed_time > 0 ? opts.low_speed_time : 0);
    po.min_segment_bytes = 0;   // 库默认 1MiB
    po.user_agent = s_ua.empty() ? nullptr : s_ua.c_str();
    po.referer = s_ref.empty() ? nullptr : s_ref.c_str();
    po.proxy = s_proxy.empty() ? nullptr : s_proxy.c_str();
    po.headers = s_headers.empty() ? nullptr : s_headers.c_str();
    po.flags = 0;
    if (opts.verify_ssl) po.flags |= PJF_FLAG_VERIFY_SSL;
    if (opts.verbose) po.flags |= PJF_FLAG_VERBOSE;
    if (opts.resume) po.flags |= PJF_FLAG_RESUME;
    po.engine_min_threads = 0;
    po.engine_max_threads = 0;

    pjf_downloader_t dl = pjf_downloader_create(&po);
    if (!dl) {
        std::fprintf(stderr, "wget-curl: failed to create downloader (bad URL or options): %s\n", opts.url.c_str());
        dld_remove(out);
        ses_append_end("failed:CREATE", opts.url, out.string(), 0);
        return 4;
    }

    pjf_task_t task = pjf_download_start(dl, nullptr, on_progress, nullptr, on_status, nullptr);
    if (!task) {
        std::fprintf(stderr, "wget-curl: failed to start download\n");
        pjf_downloader_destroy(dl);
        dld_remove(out);
        ses_append_end("failed:START", opts.url, out.string(), 0);
        return 4;
    }

    bool progress_on = !opts.quiet &&
                       (opts.progress == 1 || (opts.progress == -1 && pjf_cli::console_is_tty()));
    pjf_cli::progress_view view(progress_on);

    bool cancel_sent = false;
    pjf_err werr = PJF_OK;
    while ((werr = pjf_download_wait(task, 200)) == PJF_ERR_TIMEOUT) {
        if (pjf_cli::g_cancel_flag.load() && !cancel_sent) {
            pjf_download_cancel(task);
            cancel_sent = true;
        }
        pjf_progress prog{};
        if (pjf_query_progress(task, &prog) == PJF_OK) view.update(prog, leaf);
    }
    view.clear();

    pjf_result res{};
    if (pjf_download_result(task, &res) != PJF_OK) {
        res.err = werr;
        std::snprintf(res.message, sizeof res.message, "%s", err_name(werr));
    }

    int code = exit_code_for(res.err);
    if (res.err == PJF_OK) {
        fs::path final_out = out;
        // 无 -O 时按 Content-Disposition 重命名：契约无预开文件钩子，只能完成后改名
        if (!have_o) {
            char cd[1024] = {0};
            if (pjf_get_header(task, "Content-Disposition", cd, sizeof cd) > 0) {
                std::string cdname = pjf_cli::filename_from_content_disposition(cd);
                if (!cdname.empty() && cdname != leaf) {
                    fs::path target = pjf_cli::unique_path(out.parent_path() / cdname);
                    fs::rename(final_out, target, ec);
                    if (!ec) {
                        final_out = target;
                    } else {
                        std::fprintf(stderr, "wget-curl: warning: rename to '%s' failed, keeping '%s'\n",
                                     target.string().c_str(), final_out.string().c_str());
                    }
                }
            }
        }
        if (!opts.quiet) {
            char end[256];
            std::snprintf(end, sizeof end, "saved '%s' (%s, %d segments, %.0fs)",
                          final_out.filename().string().c_str(),
                          pjf_cli::human_bytes(double(res.bytes)).c_str(),
                          res.segments_used, res.seconds);
            std::printf("%s\n", end);
        }
        dld_remove(out);
        ses_append_end("ok", opts.url, final_out.string(), (long long)res.bytes);
    } else {
        std::fprintf(stderr, "wget-curl: %s (err=%s code=%d http=%d)\n",
                     res.message, err_name(res.err), int(res.err), res.http_status);
        if (res.err == PJF_ERR_CANCELED && !opts.quiet)
            std::fprintf(stderr, "state saved; re-run with -c to resume\n");
        std::string status = (res.err == PJF_ERR_CANCELED)
                                 ? "canceled"
                                 : std::string("failed:") + err_name(res.err);
        ses_append_end(status.c_str(), opts.url, out.string(), (long long)res.bytes);
        // 失败/取消保留 .dld 供 -c 参考（仅成功删除）
    }

    // task 在整个结果处理期间有效，故最后统一销毁（内部等全部任务结束）
    pjf_downloader_destroy(dl);
    return code;
}
