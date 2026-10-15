// fetch_smoke — libpjfetch 程序化冒烟（链接 pjfetch::pjfetch；非 0 退出 = 失败）。
//
// 服务器：默认读环境变量 PJF_SMOKE_SERVER_PORT 连接已在跑的服务（如 cli 的
// range_server.py）；未设置时用内联 python 起临时服务器（不落盘文件），
// 语义：/f.bin?size=N[&slow=1][&norange=1][&collapse=1]、/missing=404，
// 内容确定性 pattern：byte[i] = (i*31+7) & 0xFF，ETag 固定，206 诚实支持 Range。
//
// 用例：1) 小文件自动单段 2) 多段 206 + 字节精确 3) 无 Range 回退（probe 检出）
// 4) 200-collapse（probe 过、分段时 200）5) 取消+断点续传 6) 未知大小慢速单流
// 7) 404 8) 不可达端口重试。
#include <pjfetch/pjfetch.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "test_spawn.hpp"
#include <thread>
#include <vector>

namespace {
int g_failures = 0;
const char* g_tmpdir = "fetch_smoke_tmp";

void check(bool ok, const std::string& what) {
    if (ok) {
        std::printf("  [PASS] %s\n", what.c_str());
    } else {
        ++g_failures;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
}

std::string err_text(pjf_task_t t, pjf_err e) {
    static char buf[512];
    if (t && pjf_last_error(t, buf, sizeof(buf)) > 0) return buf;
    switch (e) {
    case PJF_ERR_CANCELED: return "canceled";
    case PJF_ERR_HTTP_STATUS: return "http status";
    case PJF_ERR_TOO_MANY_RETRIES: return "too many retries";
    default: return "err " + std::to_string(static_cast<int>(e));
    }
}

std::vector<unsigned char> expected_pattern(size_t n) {
    std::vector<unsigned char> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<unsigned char>((i * 31 + 7) & 0xFF);
    return v;
}

std::vector<unsigned char> read_file(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> v(static_cast<size_t>(sz));
    size_t got = sz > 0 ? std::fread(v.data(), 1, v.size(), f) : 0;
    std::fclose(f);
    v.resize(got);
    return v;
}

bool pattern_ok(const std::vector<unsigned char>& data, const char** first_bad) {
    const std::vector<unsigned char>& exp = expected_pattern(data.size());
    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] != exp[i]) {
            *first_bad = nullptr;
            return false;
        }
    }
    return true;
}

bool wait_done(pjf_task_t t, uint32_t max_ms = 90000) {
    uint32_t waited = 0;
    while (pjf_download_wait(t, 500) == PJF_ERR_TIMEOUT) {
        waited += 500;
        if (waited >= max_ms) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 内联临时服务器（仅在未设置 PJF_SMOKE_SERVER_PORT 时启动；不作为文件提交）
// ---------------------------------------------------------------------------
const char* PY_SERVER =
    "import http.server,socketserver,threading,time,os,sys\n"
    "P=int(sys.argv[1])\n"
    "S={};BLK=bytes((i*31+7)&255 for i in range(256));C={}\n"  // pattern 周期 256
    "def mk(n):\n"
    " if n not in C:C[n]=BLK*(n//256)+BLK[:n%256]\n"
    " return C[n]\n"
    "class H(http.server.BaseHTTPRequestHandler):\n"
    " protocol_version='HTTP/1.1'\n"
    " def log_message(s,*a):pass\n"
    " def q(s):\n"
    "  u=s.path;qa={}\n"
    "  if '?' in u:\n"
    "   for kv in u.split('?',1)[1].split('&'):\n"
    "    k,_,v=kv.partition('=');qa[k]=v\n"
    "  return qa\n"
    " def body(s):return mk(int(s.q().get('size','1048576')))\n"
    " def hdrs(s,n):\n"
    "  s.send_response(200);s.send_header('ETag','\"smoke\"');\n"
    "  s.send_header('Content-Length',str(n));s.end_headers()\n"
    " def do_HEAD(s):\n"
    "  if s.path.startswith('/missing'):s.send_error(404);return\n"
    "  if s.q().get('slow')=='1':s.send_response(200);s.send_header('ETag','\"smoke\"');s.send_header('Connection','close');s.end_headers();return\n"
    "  s.hdrs(len(s.body()))\n"
    " def do_GET(s):\n"
    "  if s.path.startswith('/missing'):s.send_error(404);return\n"
    "  b=s.body();qa=s.q();rng=s.headers.get('Range')\n"
    "  if qa.get('slow')=='1':\n"
    "   s.send_response(200);s.send_header('ETag','\"smoke\"');s.send_header('Connection','close');s.end_headers()\n"
    "   for i in range(0,len(b),65536):s.wfile.write(b[i:i+65536]);s.wfile.flush();time.sleep(0.08)\n"
    "   return\n"
    "  if qa.get('norange')=='1' or (qa.get('collapse')=='1' and rng and S.get(s.path,0)>=1 and not rng.startswith('bytes=0-0')):\n"
    "   S[s.path]=S.get(s.path,0)+1\n"
    "   s.send_response(200);s.send_header('ETag','\"smoke\"');s.send_header('Content-Length',str(len(b)));s.end_headers();s.wfile.write(b);return\n"
    "  if rng:\n"
    "   S[s.path]=S.get(s.path,0)+1\n"
    "   a,_,c=rng[6:].partition('-');a=int(a);c=int(c) if c else len(b)-1\n"
    "   part=b[a:c+1]\n"
    "   s.send_response(206);s.send_header('ETag','\"smoke\"');s.send_header('Content-Range','bytes %d-%d/%d'%(a,c,len(b)));s.send_header('Content-Length',str(len(part)));s.end_headers()\n"
    "   if qa.get('rate')=='1':\n"  // 限速 206：已知大小 + 慢速，供取消/续传用例
    "    for i in range(0,len(part),16384):s.wfile.write(part[i:i+16384]);s.wfile.flush();time.sleep(0.15)\n"
    "   else:s.wfile.write(part)\n"
    "   return\n"
    "  s.hdrs(len(b));s.wfile.write(b)\n"
    "srv=socketserver.ThreadingTCPServer(('127.0.0.1',P),H);srv.daemon_threads=True\n"
    "sys.stdout=sys.stderr=open(sys.argv[3],'a',buffering=1,encoding='utf-8',errors='replace')\n"
    "open(sys.argv[2],'w').close()\n"   // 就绪标记：绑定成功后写文件，测试端轮询它

    "threading.Thread(target=lambda:(time.sleep(300),os._exit(0)),daemon=True).start()\n"
    "srv.serve_forever()\n";

int server_port() {
    if (const char* p = std::getenv("PJF_SMOKE_SERVER_PORT")) return std::atoi(p);
    static int port = 18123 + (static_cast<int>(std::chrono::steady_clock::now().time_since_epoch().count() % 900));

    // 把脚本落盘再启动：脚本内含字面双引号（如 "smoke"），直接拼进
    // 命令行会破坏引号配对；落盘同时也便于失败时查看子进程日志。
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) dir = ".";
    std::filesystem::path script = dir / ("pjf_smoke_server_" + std::to_string(port) + ".py");
    std::filesystem::path log = dir / ("pjf_smoke_server_" + std::to_string(port) + ".log");
    std::filesystem::path ready = dir / ("pjf_smoke_server_" + std::to_string(port) + ".ready");
    std::error_code ec2;
    std::filesystem::remove(ready, ec2);
    {
        std::ofstream f(script, std::ios::binary | std::ios::trunc);
        f << PY_SERVER;
    }

    // 启动内联服务器：无窗口、不继承句柄（服务器自行把输出写到 --log 文件，见 PY_SERVER）。
    // 继承句柄会让捕获式运行器（ctest）永远等不到管道 EOF；可见窗口会打扰使用者。
    std::string q = "\"" + script.string() + "\"";   // 路径可能含空格
    std::string lg = "\"" + log.string() + "\"";
    std::string rd = "\"" + ready.string() + "\"";
    std::string cmd = "python -u " + q + " " + std::to_string(port) + " " + rd + " " + lg;
    pjf_test::spawn_detached(cmd);

    // 就绪轮询（最多 ~10s）：固定 sleep 在慢机器/冷启动下不可靠；
    // 服务器绑定端口成功后写 ready 文件（避免在此引入 socket 平台代码）
    for (int i = 0; i < 100; ++i) {
        if (std::filesystem::exists(ready, ec2)) return port;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::printf("  [WARN] inline server did not come up on port %d; log follows:\n", port);
    if (std::FILE* f = std::fopen(log.string().c_str(), "rb")) {
        char buf[1024];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) std::fwrite(buf, 1, n, stdout);
        std::fclose(f);
    }
    return port;
}

pjf_options base_opts(const std::string& url) {
    pjf_options o{};
    o.url = url.c_str();
    o.output_path = nullptr;
    o.N_segments = 8;
    o.max_retry = 3;
    o.connect_timeout_ms = 5000;
    o.timeout_ms = 0;
    o.low_speed_limit = 512;
    o.low_speed_secs = 10;
    o.min_segment_bytes = 64 * 1024;  // 便于小文件也走多段
    o.flags = 0;
    return o;
}
} // namespace

int main() {
    // 无缓冲：重定向到文件时也能实时观察进度，便于定位挂起点
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("fetch_smoke start\n");
    std::error_code ec;
    std::filesystem::remove_all(g_tmpdir, ec);
    std::filesystem::create_directories(g_tmpdir, ec);

    int port = server_port();
    char base[128];
    std::snprintf(base, sizeof(base), "http://127.0.0.1:%d", port);
    std::printf("server base: %s\n", base);

    // ---- 用例 1：小文件自动单段 ----
    {
        std::printf("[case1] small file single segment\n");
        pjf_options o = base_opts(std::string(base) + "/f.bin?size=1024");
        std::string out = std::string(g_tmpdir) + "/small.bin";
        o.output_path = out.c_str();
        pjf_downloader_t d = pjf_downloader_create(&o);
        check(d != nullptr, "downloader create");
        pjf_task_t t = pjf_download_start(d, nullptr, nullptr, nullptr, nullptr, nullptr);
        check(t != nullptr, "start");
        check(wait_done(t), "wait");
        pjf_result r{};
        check(pjf_download_result(t, &r) == PJF_OK && r.err == PJF_OK, "result ok: " + err_text(t, r.err));
        check(r.segments_used == 1, "segments_used==1 (got " + std::to_string(r.segments_used) + ")");
        check(r.bytes == 1024, "bytes==1024");
        std::vector<unsigned char> data = read_file(out);
        const char* bad = nullptr;
        check(data.size() == 1024 && pattern_ok(data, &bad), "content pattern match");
        pjf_downloader_destroy(d);
    }

    // ---- 用例 2：多段 206 + 字节精确 ----
    {
        std::printf("[case2] multi-segment 206 exact bytes\n");
        pjf_options o = base_opts(std::string(base) + "/f.bin?size=8388608");  // 8MiB
        o.N_segments = 8;
        std::string out = std::string(g_tmpdir) + "/multi.bin";
        o.output_path = out.c_str();
        pjf_downloader_t d = pjf_downloader_create(&o);
        pjf_task_t t = pjf_download_start(d, nullptr, nullptr, nullptr, nullptr, nullptr);
        check(wait_done(t), "wait");
        pjf_result r{};
        check(pjf_download_result(t, &r) == PJF_OK && r.err == PJF_OK, "result ok: " + err_text(t, r.err));
        check(r.segments_used == 8, "segments_used==8 (got " + std::to_string(r.segments_used) + ")");
        check(r.range_used == 1, "range_used==1");
        check(r.bytes == 8388608, "bytes==8MiB");
        std::vector<unsigned char> data = read_file(out);
        const char* bad = nullptr;
        check(data.size() == 8388608 && pattern_ok(data, &bad), "multi-stream content byte-exact");
        // 分段快照 API 冒烟
        pjf_segment segs[8];
        check(pjf_query_segments(t, segs, 8) == 8, "query_segments==8");
        // 响应头 API 冒烟
        char hdr[128];
        check(pjf_get_header(t, "ETag", hdr, sizeof(hdr)) > 0, "get_header ETag");
        pjf_downloader_destroy(d);
    }

    // ---- 用例 3：无 Range 回退（probe 检出，单流完成）----
    {
        std::printf("[case3] no-range fallback\n");
        pjf_options o = base_opts(std::string(base) + "/f.bin?size=2097152&norange=1");
        std::string out = std::string(g_tmpdir) + "/norange.bin";
        o.output_path = out.c_str();
        pjf_downloader_t d = pjf_downloader_create(&o);
        pjf_task_t t = pjf_download_start(d, nullptr, nullptr, nullptr, nullptr, nullptr);
        check(wait_done(t), "wait");
        pjf_result r{};
        check(pjf_download_result(t, &r) == PJF_OK && r.err == PJF_OK, "result ok: " + err_text(t, r.err));
        check(r.range_used == 0, "range_used==0");
        check(r.segments_used == 1, "segments_used==1");
        std::vector<unsigned char> data = read_file(out);
        const char* bad = nullptr;
        check(data.size() == 2097152 && pattern_ok(data, &bad), "content pattern match");
        pjf_downloader_destroy(d);
    }

    // ---- 用例 4：200-collapse（probe 见过 206，分段请求被服务器 200 全量忽略）----
    {
        std::printf("[case4] 200-collapse restart as single stream\n");
        pjf_options o = base_opts(std::string(base) + "/f.bin?size=1048576&collapse=1");
        std::string out = std::string(g_tmpdir) + "/collapse.bin";
        o.output_path = out.c_str();
        pjf_downloader_t d = pjf_downloader_create(&o);
        pjf_task_t t = pjf_download_start(d, nullptr, nullptr, nullptr, nullptr, nullptr);
        check(wait_done(t), "wait");
        pjf_result r{};
        check(pjf_download_result(t, &r) == PJF_OK && r.err == PJF_OK, "result ok: " + err_text(t, r.err));
        check(r.range_used == 0, "range_used==0 after collapse");
        check(r.segments_used == 1, "segments_used==1 after collapse");
        std::vector<unsigned char> data = read_file(out);
        const char* bad = nullptr;
        check(data.size() == 1048576 && pattern_ok(data, &bad), "content pattern match after collapse");
        pjf_downloader_destroy(d);
    }

    // ---- 用例 5：取消 + 断点续传 ----
    {
        std::printf("[case5] cancel + resume\n");
        std::string url = std::string(base) + "/f.bin?size=4194304&rate=1";  // 已知大小限速多段
        std::string out = std::string(g_tmpdir) + "/resume.bin";
        std::string part = out + ".part";
        std::string meta = out + ".pjmeta";

        int progress_seen = 0;
        auto pcb = [](const pjf_progress* p, void* ud) {
            (void)p;
            ++*static_cast<int*>(ud);
        };

        {
            pjf_options o = base_opts(url);
            o.output_path = out.c_str();
            o.timeout_ms = 0;
            pjf_downloader_t d = pjf_downloader_create(&o);
            pjf_task_t t = pjf_download_start(d, nullptr, pcb, &progress_seen, nullptr, nullptr);
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            pjf_download_cancel(t);
            check(wait_done(t), "wait after cancel");
            pjf_result r{};
            check(pjf_download_result(t, &r) == PJF_OK && r.err == PJF_ERR_CANCELED,
                  "canceled result: " + err_text(t, r.err));
            check(std::filesystem::exists(part, ec), ".part exists");
            check(std::filesystem::exists(meta, ec), ".pjmeta exists");
            check(progress_seen > 0, "progress callbacks fired");
            pjf_downloader_destroy(d);
        }
        check(!std::filesystem::exists(out, ec), "final out not created on cancel");
        check(std::filesystem::file_size(part, ec) == 4194304, ".part preallocated to total");
        {
            pjf_options o = base_opts(url);
            o.output_path = out.c_str();
            o.flags = PJF_FLAG_RESUME;
            pjf_downloader_t d = pjf_downloader_create(&o);
            pjf_task_t t = pjf_download_start(d, nullptr, nullptr, nullptr, nullptr, nullptr);
            check(wait_done(t, 120000), "resume wait");
            pjf_result r{};
            check(pjf_download_result(t, &r) == PJF_OK && r.err == PJF_OK,
                  "resume result ok: " + err_text(t, r.err));
            check(r.resumed == 1, "resumed==1");
            std::vector<unsigned char> data = read_file(out);
            const char* bad = nullptr;
            check(data.size() == 4194304 && pattern_ok(data, &bad), "resumed content pattern match");
            check(!std::filesystem::exists(part, ec), ".part renamed away on success");
            check(!std::filesystem::exists(meta, ec), ".pjmeta removed on success");
            pjf_downloader_destroy(d);
        }
    }

    // ---- 用例 6：未知大小慢速单流（无 Content-Length）----
    {
        std::printf("[case6] unknown-size streaming\n");
        pjf_options o = base_opts(std::string(base) + "/f.bin?size=524288&slow=1");
        std::string out = std::string(g_tmpdir) + "/stream.bin";
        o.output_path = out.c_str();
        pjf_downloader_t d = pjf_downloader_create(&o);
        pjf_task_t t = pjf_download_start(d, nullptr, nullptr, nullptr, nullptr, nullptr);
        check(wait_done(t, 120000), "wait");
        pjf_result r{};
        check(pjf_download_result(t, &r) == PJF_OK && r.err == PJF_OK, "result ok: " + err_text(t, r.err));
        check(r.segments_used == 1, "segments_used==1");
        std::vector<unsigned char> data = read_file(out);
        const char* bad = nullptr;
        check(data.size() == 524288 && pattern_ok(data, &bad), "stream content pattern match");
        pjf_downloader_destroy(d);
    }

    // ---- 用例 7：404 ----
    {
        std::printf("[case7] 404\n");
        pjf_options o = base_opts(std::string(base) + "/missing");
        std::string out = std::string(g_tmpdir) + "/missing.bin";
        o.output_path = out.c_str();
        pjf_downloader_t d = pjf_downloader_create(&o);
        pjf_task_t t = pjf_download_start(d, nullptr, nullptr, nullptr, nullptr, nullptr);
        check(wait_done(t), "wait");
        pjf_result r{};
        check(pjf_download_result(t, &r) == PJF_OK && r.err == PJF_ERR_HTTP_STATUS,
              "404 => HTTP_STATUS (err=" + std::to_string(static_cast<int>(r.err)) + " " + err_text(t, r.err) + ")");
        pjf_downloader_destroy(d);
    }

    // ---- 用例 8：不可达端口（重试后失败）----
    {
        std::printf("[case8] unreachable port retries\n");
        pjf_options o = base_opts("http://127.0.0.1:1/f.bin");  // 端口 1 通常无监听
        o.max_retry = 2;
        o.connect_timeout_ms = 2000;
        std::string out = std::string(g_tmpdir) + "/unreach.bin";
        o.output_path = out.c_str();
        pjf_downloader_t d = pjf_downloader_create(&o);
        pjf_task_t t = pjf_download_start(d, nullptr, nullptr, nullptr, nullptr, nullptr);
        check(wait_done(t, 60000), "wait");
        pjf_result r{};
        check(pjf_download_result(t, &r) == PJF_OK && r.err != PJF_OK,
              "unreachable => failure (err=" + std::to_string(static_cast<int>(r.err)) + ")");
        pjf_downloader_destroy(d);
    }

    std::printf("fetch_smoke done: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
