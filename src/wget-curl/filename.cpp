#include "filename.hpp"

#include <cstring>

namespace pjf_cli {

namespace {

int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int ci_compare(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = char(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = char(cb - 'A' + 'a');
        if (ca != cb) return ca < cb ? -1 : 1;
        ++a; ++b;
    }
    if (*a) return 1;
    if (*b) return -1;
    return 0;
}

bool is_reserved_stem(const std::string& stem) {
    static const char* names[] = {"CON","PRN","AUX","NUL",
        "COM1","COM2","COM3","COM4","COM5","COM6","COM7","COM8","COM9",
        "LPT1","LPT2","LPT3","LPT4","LPT5","LPT6","LPT7","LPT8","LPT9"};
    for (const char* n : names)
        if (ci_compare(stem.c_str(), n) == 0) return true;
    return false;
}

// percent-decode；非法序列原样保留。
std::string pct_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hex_val(s[i + 1]), lo = hex_val(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(char((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

} // namespace

std::string sanitize_filename(const std::string& raw) {
    std::string s = raw;
    for (char& c : s) {
        if (static_cast<unsigned char>(c) < 0x20 || std::strchr("<>:\"/\\|?*", c)) c = '_';
    }
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return {};
    s = s.substr(b);
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();  // Windows 目录语义
    if (s.empty()) return {};

    // 保留设备名（"CON.txt" 也命中）
    size_t dot = s.find('.');
    std::string stem = (dot == std::string::npos) ? s : s.substr(0, dot);
    if (is_reserved_stem(stem)) s = stem + "_" + s.substr(stem.size());

    if (s.size() > 200) {
        size_t d = s.rfind('.');
        if (d != std::string::npos && d > 0 && s.size() - d < 40)
            s = s.substr(0, 200 - (s.size() - d)) + s.substr(d);
        else
            s = s.substr(0, 200);
    }
    return s;
}

std::string filename_from_url(const std::string& url) {
    size_t scheme_end = url.find("://");
    size_t authority_begin = (scheme_end == std::string::npos) ? 0 : scheme_end + 3;
    size_t path_slash = url.find('/', authority_begin);
    size_t path_begin = (path_slash == std::string::npos) ? url.size() : path_slash + 1;
    size_t end = url.size();
    for (size_t i = path_begin; i < url.size(); ++i)
        if (url[i] == '?' || url[i] == '#') { end = i; break; }
    if (path_begin >= end) return {};  // 以 '/' 结尾或无 path
    size_t last_slash = url.rfind('/', end);
    size_t begin = (last_slash != std::string::npos && last_slash >= authority_begin)
                       ? last_slash + 1 : path_begin;
    if (begin >= end) return {};
    return sanitize_filename(pct_decode(url.substr(begin, end - begin)));
}

std::string filename_from_content_disposition(const std::string& cd) {
    std::string lower_cd;
    lower_cd.reserve(cd.size());
    for (char c : cd) lower_cd.push_back((c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c);

    auto find_param = [&](const char* name) -> std::string {
        size_t nlen = std::strlen(name);
        size_t p = 0;
        while ((p = lower_cd.find(name, p)) != std::string::npos) {
            char prev = (p == 0) ? ';' : lower_cd[p - 1];
            if (prev == ';' || prev == ',' || prev == ' ') {
                size_t eq = lower_cd.find('=', p + nlen);
                if (eq != std::string::npos) {
                    size_t b = cd.find_first_not_of(" \t", eq + 1);
                    if (b == std::string::npos) return {};
                    if (cd[b] == '"') {
                        size_t e = cd.find('"', b + 1);
                        if (e == std::string::npos) return cd.substr(b + 1);
                        return cd.substr(b + 1, e - b - 1);
                    }
                    size_t e = cd.find(';', b);
                    std::string v = (e == std::string::npos) ? cd.substr(b) : cd.substr(b, e - b);
                    size_t last = v.find_last_not_of(" \t");
                    return (last == std::string::npos) ? std::string() : v.substr(0, last + 1);
                }
            }
            p += nlen;
        }
        return {};
    };

    std::string v = find_param("filename*");       // RFC5987 优先
    if (v.empty()) v = find_param("filename");
    if (v.empty()) return {};
    // 形如 UTF-8''percent%20encoded —— 剥 charset 与语言前缀
    size_t tick1 = v.find('\'');
    if (tick1 != std::string::npos) {
        size_t tick2 = v.find('\'', tick1 + 1);
        if (tick2 != std::string::npos) v = v.substr(tick2 + 1);
    }
    v = pct_decode(v);
    size_t slash = v.find_last_of("/\\");          // 只取 basename，防服务器给路径
    if (slash != std::string::npos) v = v.substr(slash + 1);
    return sanitize_filename(v);
}

std::filesystem::path unique_path(const std::filesystem::path& p) {
    if (!std::filesystem::exists(p)) return p;
    std::filesystem::path dir = p.parent_path();
    std::filesystem::path stem = p.stem();
    std::filesystem::path ext = p.extension();
    for (int n = 1;; ++n) {
        std::filesystem::path cand = dir / (stem.string() + " (" + std::to_string(n) + ")" + ext.string());
        if (!std::filesystem::exists(cand)) return cand;
    }
}

} // namespace pjf_cli
