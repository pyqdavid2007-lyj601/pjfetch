#ifndef WGET_CURL_FILENAME_HPP
#define WGET_CURL_FILENAME_HPP

// 输出文件名推导（纯 std::string/std::filesystem，无平台 API）。规则（本文即权威定义）：
//   URL -> 文件名：取 path 末段（'/' 分隔），先剥 query('?'起)/fragment('#'起)，
//   再 percent-decode，再经 sanitize。
//   sanitize：替换 Windows 非法字符 <>:"/\|?* 与 C0 控制符(0x00-0x1F) 为 '_'；
//   剥首尾空白与结尾的 '.'/' '（Windows 目录语义）；stem 命中 Windows 保留设备名
//   （CON/PRN/AUX/NUL/COM1-9/LPT1-9，大小写不敏感）时追加 '_'；限长 200 字节并尽量
//   保留扩展名；空结果返回空串（调用方给 download.bin）。
//   Content-Disposition：优先 filename*=RFC5987（percent-decode，charset 前缀忽略按
//   UTF-8 处理），否则 filename=；只取 basename。

#include <filesystem>
#include <string>

namespace pjf_cli {

std::string sanitize_filename(const std::string& raw);
std::string filename_from_url(const std::string& url);
std::string filename_from_content_disposition(const std::string& cd_value);

// 为重命名/落盘选择不与现有文件冲突的目标路径：p 存在时插入 " (N)"（N=1,2,...）。
std::filesystem::path unique_path(const std::filesystem::path& p);

} // namespace pjf_cli

#endif // WGET_CURL_FILENAME_HPP
