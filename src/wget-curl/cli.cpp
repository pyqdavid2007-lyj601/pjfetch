#include "cli.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pjf_cli {

const char* version_string() { return "wget-curl (pjfetch) 0.1.0"; }

std::string usage_text() {
    return
        "Usage: wget-curl [para-jobs=N] [options] <url>\n"
        "\n"
        "First argument: para-jobs=<N>   number of parallel segments (1..32, default 8)\n"
        "\n"
        "Options:\n"
        "  -O path                    write output to exact path\n"
        "  -P dir                     save under directory prefix\n"
        "  -c                         resume partial download (default on)\n"
        "  --no-resume                disable resume\n"
        "  --retry N                  max retries per segment (default 5)\n"
        "  --timeout S                overall transfer timeout seconds (0 = none)\n"
        "  --connect-timeout S        connect timeout seconds (default 15)\n"
        "  --low-speed-limit B        abort if slower than B bytes/s\n"
        "  --low-speed-time S         ...sustained for S seconds\n"
        "  -U, --user-agent S         user-agent string\n"
        "  --header \"K: V\"            extra request header (repeatable)\n"
        "  --referer S                referer string\n"
        "  --proxy S                  proxy URL\n"
        "  --no-check-certificate     skip TLS certificate verification\n"
        "  -q                         quiet (no progress, no status line)\n"
        "  -v                         verbose libcurl output to stderr\n"
        "  --show-progress / --no-progress\n"
        "                             force progress bar on/off (default: auto on tty)\n"
        "  -h, --help                 this help\n"
        "  --version                  print version\n"
        "\n"
        "Config defaults: ~/.pjfetch/pj-cfg.cnf ([global] section, key = long option name)\n";
}

// ---------- 配置默认值 ----------

static bool parse_bool(const std::string& v, bool dflt) {
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return dflt;
}

static bool parse_long(const std::string& s, long& out) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (errno || !end || *end) return false;
    out = v;
    return true;
}

int apply_config_defaults(const std::map<std::string, std::string>& cfg, options& o) {
    int unknown = 0;
    for (const auto& kv : cfg) {
        const std::string& k = kv.first;
        if (k == "para-jobs" || k == "retry" || k == "timeout" || k == "connect-timeout" ||
            k == "low-speed-limit" || k == "low-speed-time") {
            long v;
            if (!parse_long(kv.second, v)) { ++unknown; continue; }
            if (k == "para-jobs") o.para_jobs = int(v);
            else if (k == "retry") o.retry = int(v);
            else if (k == "timeout") o.timeout = v;
            else if (k == "connect-timeout") o.connect_timeout = v;
            else if (k == "low-speed-limit") o.low_speed_limit = v;
            else o.low_speed_time = v;
        } else if (k == "user-agent") o.user_agent = kv.second;
        else if (k == "referer") o.referer = kv.second;
        else if (k == "proxy") o.proxy = kv.second;
        else if (k == "header") { if (!o.headers.empty()) o.headers += "\r\n"; o.headers += kv.second; }
        else if (k == "output") o.output = kv.second;
        else if (k == "dir") o.dir = kv.second;
        else if (k == "resume") o.resume = parse_bool(kv.second, o.resume);
        else if (k == "no-check-certificate") o.verify_ssl = !parse_bool(kv.second, false);
        else if (k == "quiet") o.quiet = parse_bool(kv.second, o.quiet);
        else if (k == "verbose") o.verbose = parse_bool(kv.second, o.verbose);
        else if (k == "show-progress") o.progress = parse_bool(kv.second, false) ? 1 : 0;
        else if (k == "no-progress") o.progress = parse_bool(kv.second, false) ? 0 : o.progress;
        else ++unknown;
    }
    return unknown;
}

// ---------- 解析 ----------

namespace {

// has_inline 时 inline_val 可为空串（--opt=），否则消费下一参数
bool take_value(bool has_inline, const std::string& inline_val,
                const std::vector<std::string>& argv, size_t& i,
                std::string& out, std::string& err, const char* optname) {
    if (has_inline) { out = inline_val; return true; }
    if (i + 1 >= argv.size()) {
        err = std::string("option ") + optname + " requires a value";
        return false;
    }
    out = argv[++i];
    return true;
}

bool to_long_strict(const std::string& s, long& out) {
    return parse_long(s, out);
}

} // namespace

parse_status parse(const std::vector<std::string>& argv, const options& defaults,
                   options& out, std::string& err, std::string& text) {
    out = defaults;
    size_t i = 0;

    if (!argv.empty()) {
        const std::string& first = argv[0];
        bool is_pj = first.rfind("para-jobs=", 0) == 0;
        if (is_pj) {
            long v;
            if (!to_long_strict(first.substr(10), v)) {
                err = "invalid para-jobs value '" + first.substr(10) + "'";
                return parse_status::error;
            }
            out.para_jobs = int(v);
            i = 1;
        }
    }

    bool have_url = false;
    for (; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--") {  // 其余全部按位置参数
            for (++i; i < argv.size(); ++i) {
                if (have_url) { err = "unexpected extra argument '" + argv[i] + "'"; return parse_status::error; }
                out.url = argv[i]; have_url = true;
            }
            break;
        }
        if (arg.size() >= 2 && arg[0] == '-' && arg[1] == '-') {
            std::string name = arg, val;
            size_t eq = arg.find('=');
            if (eq != std::string::npos) { name = arg.substr(0, eq); val = arg.substr(eq + 1); }
            std::string* slot = nullptr;
            if (name == "--output") slot = &out.output;
            else if (name == "--dir") slot = &out.dir;
            else if (name == "--user-agent") slot = &out.user_agent;
            else if (name == "--referer") slot = &out.referer;
            else if (name == "--proxy") slot = &out.proxy;
            if (slot) {
                if (eq == std::string::npos) {
                    if (!take_value(false, "", argv, i, *slot, err, name.c_str()))
                        return parse_status::error;
                } else *slot = val;
                continue;
            }
            std::string v;
            if (name == "--retry" || name == "--timeout" || name == "--connect-timeout" ||
                name == "--low-speed-limit" || name == "--low-speed-time") {
                if (!take_value(eq != std::string::npos, val, argv, i, v, err, name.c_str()))
                    return parse_status::error;
                long n;
                if (!to_long_strict(v, n)) { err = "invalid numeric value for " + name + ": '" + v + "'"; return parse_status::error; }
                if (name == "--retry") out.retry = int(n);
                else if (name == "--timeout") out.timeout = n;
                else if (name == "--connect-timeout") out.connect_timeout = n;
                else if (name == "--low-speed-limit") out.low_speed_limit = n;
                else out.low_speed_time = n;
            } else if (name == "--header") {
                if (!take_value(eq != std::string::npos, val, argv, i, v, err, name.c_str()))
                    return parse_status::error;
                if (!out.headers.empty()) out.headers += "\r\n";
                out.headers += v;
            } else if (name == "--no-resume") out.resume = false;
            else if (name == "--no-check-certificate") out.verify_ssl = false;
            else if (name == "--show-progress") out.progress = 1;
            else if (name == "--no-progress") out.progress = 0;
            else if (name == "--help") { text = usage_text(); return parse_status::help; }
            else if (name == "--version") { text = std::string(version_string()) + "\n"; return parse_status::version; }
            else { err = "unknown option '" + name + "'"; return parse_status::error; }
            continue;
        }
        if (arg.size() >= 2 && arg[0] == '-' && arg != "-") {
            for (size_t c = 1; c < arg.size(); ++c) {
                char ch = arg[c];
                if (ch == 'c') out.resume = true;
                else if (ch == 'q') out.quiet = true;
                else if (ch == 'v') out.verbose = true;
                else if (ch == 'h') { text = usage_text(); return parse_status::help; }
                else if (ch == 'O' || ch == 'P' || ch == 'U') {
                    std::string rest = arg.substr(c + 1);
                    std::string& slot = (ch == 'O') ? out.output : (ch == 'P') ? out.dir : out.user_agent;
                    if (!rest.empty()) {
                        slot = rest;
                    } else {
                        if (i + 1 >= argv.size()) {
                            err = std::string("option -") + ch + " requires a value";
                            return parse_status::error;
                        }
                        slot = argv[++i];
                    }
                    c = arg.size();  // 值吞掉簇剩余部分
                } else {
                    err = std::string("unknown option '-") + ch + "'";
                    return parse_status::error;
                }
            }
            continue;
        }
        if (arg.rfind("para-jobs=", 0) == 0) {
            err = "para-jobs= is only allowed as the first argument";
            return parse_status::error;
        }
        if (have_url) { err = "unexpected extra argument '" + arg + "'"; return parse_status::error; }
        out.url = arg;
        have_url = true;
    }

    if (!have_url) { err = "missing URL"; return parse_status::error; }
    if (out.para_jobs < 1) out.para_jobs = 1;
    if (out.para_jobs > 32) out.para_jobs = 32;
    if (out.retry < 0) out.retry = 0;
    return parse_status::ok;
}

} // namespace pjf_cli
