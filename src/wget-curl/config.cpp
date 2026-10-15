#include "config.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace pjf_cli {

static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n\f\v");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n\f\v");
    return s.substr(b, e - b + 1);
}

static std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

std::filesystem::path home_dir() {
#ifdef _WIN32
    if (const char* p = std::getenv("USERPROFILE"); p && *p) return std::filesystem::path(p);
#else
    if (const char* p = std::getenv("HOME"); p && *p) return std::filesystem::path(p);
#endif
    return {};
}

std::filesystem::path app_dir() {
    std::filesystem::path h = home_dir();
    if (h.empty()) return {};
    return h / ".pjfetch";
}

std::map<std::string, std::string> load_config() {
    std::map<std::string, std::string> out;
    std::filesystem::path dir = app_dir();
    if (dir.empty()) return out;
    std::ifstream in(dir / "pj-cfg.cnf");
    if (!in) return out;  // 不存在 -> 静默内置默认

    std::string line, section;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;   // 注释
        if (t.front() == '[') {
            size_t close = t.find(']');
            if (close != std::string::npos) section = lower(t.substr(1, close - 1));
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;                   // 坏行：跳过
        std::string key = lower(trim(t.substr(0, eq)));
        std::string val = trim(t.substr(eq + 1));
        if (key.empty()) continue;
        if (section == "global" || section.empty()) out[key] = val;
    }
    return out;
}

} // namespace pjf_cli
