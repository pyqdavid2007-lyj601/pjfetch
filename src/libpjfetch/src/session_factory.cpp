// cpr 会话工厂：库内唯一创建 cpr::Session 的地方。每个会话（含探测）必须显式禁用
// Accept-Encoding——cpr 默认开全部压缩，会使字节 Range 与实际载荷错位。
#include "downloader_impl.hpp"

#include <cpr/cpr.h>

namespace pjf {

void SessionDeleter::operator()(cpr::Session* s) const { delete s; }

SessionPtr make_session(const TaskOptions& opt, const std::string& url,
                        std::shared_ptr<std::atomic_bool> cancel) {
    auto s = std::unique_ptr<cpr::Session, SessionDeleter>(new cpr::Session());
    s->SetUrl(cpr::Url{url});
    if (!opt.user_agent.empty())
        s->SetUserAgent(cpr::UserAgent{opt.user_agent});
    // proxy: "" => env 默认（不设）；"<none>" => 显式禁代理（HTTP/HTTPS 全空串）
    if (opt.proxy == "<none>")
        s->SetProxies(cpr::Proxies{{"http", ""}, {"https", ""}});
    else if (!opt.proxy.empty())
        s->SetProxies(cpr::Proxies{{"http", opt.proxy}, {"https", opt.proxy}});

    // cpr 的 SetHeader 是整表替换：Referer 与附加头必须合并后一次设置
    cpr::Header extra;
    if (!opt.referer.empty())
        extra["Referer"] = opt.referer;
    if (!opt.headers.empty()) {
        size_t pos = 0;
        while (pos < opt.headers.size()) {
            size_t eol = opt.headers.find("\r\n", pos);
            if (eol == std::string::npos) eol = opt.headers.size();
            std::string line = opt.headers.substr(pos, eol - pos);
            pos = eol + 2;
            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string k = line.substr(0, colon);
            std::string v = line.substr(colon + 1);
            size_t b = v.find_first_not_of(" \t");
            if (b != std::string::npos) v = v.substr(b);
            else v.clear();
            if (!k.empty()) extra[k] = v;
        }
    }
    if (!extra.empty())
        s->SetHeader(extra);

    s->SetRedirect(cpr::Redirect{10L, true, false, cpr::PostRedirectFlags::NONE});
    if (opt.connect_timeout_ms > 0)
        s->SetConnectTimeout(cpr::ConnectTimeout{opt.connect_timeout_ms});
    if (opt.timeout_ms > 0)
        s->SetTimeout(cpr::Timeout{opt.timeout_ms});
    if (opt.low_speed_limit > 0 && opt.low_speed_secs > 0)
        s->SetLowSpeed(cpr::LowSpeed{opt.low_speed_limit, std::chrono::seconds{opt.low_speed_secs}});
    // VERIFY_SSL 置位 = 校验，清零 = 跳过
    s->SetVerifySsl(cpr::VerifySsl{(opt.flags & PJF_FLAG_VERIFY_SSL) != 0});
    if (opt.flags & PJF_FLAG_VERBOSE)
        s->SetVerbose(cpr::Verbose{true});
    s->SetAcceptEncoding(cpr::AcceptEncoding{cpr::AcceptEncodingMethods::disabled});
    if (cancel)
        s->SetCancellationParam(cancel);
    return s;
}

} // namespace pjf
