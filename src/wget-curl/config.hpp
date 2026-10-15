#ifndef WGET_CURL_CONFIG_HPP
#define WGET_CURL_CONFIG_HPP

// 全局配置文件 ~/.pjfetch/pj-cfg.cnf —— Apache 式 Section-Key-Value（手写解析器）。
// 格式（本文即权威定义）：'#'/';' 开头的整行为注释，空行忽略，行内注释不支持；
//   [section] 名与 key 均大小写不敏感并归一为小写；key = value 两侧空白剥离，
//   value 其余（含引号）原样保留；仅 [global] 生效，其余 section 读入后忽略；
//   key = 长选项名去前缀（para-jobs=16、retry=5、resume=on ...）。
// 文件不存在 -> 返回空 map（静默用内置默认）；坏行只跳过，不致命。
// '~' 一律解析为 %USERPROFILE%（Windows）/ $HOME（POSIX）。

#include <filesystem>
#include <map>
#include <string>

namespace pjf_cli {

// 用户主目录（USERPROFILE/HOME）；未设置时返回空路径。
std::filesystem::path home_dir();

// ~/.pjfetch（不保证存在）；home 不可得时返回空路径。
std::filesystem::path app_dir();

// 读取 ~/.pjfetch/pj-cfg.cnf，返回 [global] 的 key->value（key 已归一为小写）。
std::map<std::string, std::string> load_config();

} // namespace pjf_cli

#endif // WGET_CURL_CONFIG_HPP
