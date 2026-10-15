#ifndef WGET_CURL_CLI_HPP
#define WGET_CURL_CLI_HPP

// 手写命令行解析器（无外部依赖）。语法（本文即权威定义）：
//   para-jobs=<N>  只允许出现在首个位置，钳 1..32，默认 8；出现在别处或格式非法 -> exit 2。
//   长选项 --opt val 与 --opt=val；短选项可簇合（-qv），可带值（-Ofile 或 -O file）。
//   解析错误（未知选项、缺值、多余/缺失 URL、非法数值）-> usage -> exit 2；恰好一个 URL。
// 配置默认值：load_config() 的 [global] 先行填入 options，再由显式 CLI 选项覆盖。

#include <map>
#include <string>
#include <vector>

namespace pjf_cli {

struct options {
    std::string url;
    std::string output;          // -O（精确路径；空 = 由 -P/URL 推导）
    std::string dir;             // -P（目录前缀）
    bool resume = true;          // -c 默认开；--no-resume 关
    int  retry = 5;              // --retry N
    long timeout = 0;            // --timeout S（秒；0 = 不限）
    long connect_timeout = 0;    // --connect-timeout S（0 = 库默认 15s）
    long low_speed_limit = 0;    // --low-speed-limit B
    long low_speed_time = 0;     // --low-speed-time S
    std::string user_agent;      // -U/--user-agent
    std::string referer;         // --referer
    std::string proxy;           // --proxy
    std::string headers;         // --header "K: V" 可重复，以 "\r\n" 拼接
    bool verify_ssl = true;      // 默认校验；--no-check-certificate 关
    bool quiet = false;          // -q
    bool verbose = false;        // -v
    int  progress = -1;          // -1 自动（stderr 为 tty 且非 -q 时开），0 关，1 开
    int  para_jobs = 8;          // para-jobs=<N>，钳 1..32
};

enum class parse_status { ok, help, version, error };

// argv 不含 argv[0]（由 console_command_line 保证）。
// error 时 err 填人读原因；help/version 时 text 已填对应输出文本。
parse_status parse(const std::vector<std::string>& argv, const options& defaults,
                   options& out, std::string& err, std::string& text);

// 返回未知/非法 key 数（仅提示用）。
int apply_config_defaults(const std::map<std::string, std::string>& cfg, options& o);

const char* version_string();
std::string usage_text();

} // namespace pjf_cli

#endif // WGET_CURL_CLI_HPP
