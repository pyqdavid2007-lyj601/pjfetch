// cpr::ErrorCode -> pjf_err 映射 + HTTP 状态归类
#include "downloader_impl.hpp"

#include <cpr/cpr.h>

namespace pjf {

pjf_err map_cpr_error(int cpr_code_int) {
    // int 承载 cpr::ErrorCode，避免本文件外泄漏 cpr 类型
    switch (static_cast<cpr::ErrorCode>(cpr_code_int)) {
    case cpr::ErrorCode::OK:
        return PJF_OK;
    case cpr::ErrorCode::OPERATION_TIMEDOUT:
        return PJF_ERR_TIMEOUT;
    case cpr::ErrorCode::UNSUPPORTED_PROTOCOL:
    case cpr::ErrorCode::URL_MALFORMAT:
    case cpr::ErrorCode::BAD_FUNCTION_ARGUMENT:
        return PJF_ERR_BAD_URL;
    case cpr::ErrorCode::RANGE_ERROR:
        return PJF_ERR_RANGE_UNSUPPORTED;
    case cpr::ErrorCode::PEER_FAILED_VERIFICATION:
    case cpr::ErrorCode::SSL_CACERT_BADFILE:
        return PJF_ERR_NETWORK;  // 证书类失败归网络面，消息里保留细节
    default:
        return PJF_ERR_NETWORK;
    }
}

bool http_retryable(int status) {
    return status == 429 || status >= 500;
}

pjf_err classify_http(int status) {
    (void)status;
    return PJF_ERR_HTTP_STATUS;
}

const char* pjf_err_str(pjf_err e) {
    switch (e) {
    case PJF_OK: return "ok";
    case PJF_ERR_INVALID_ARG: return "invalid argument";
    case PJF_ERR_BAD_URL: return "bad url";
    case PJF_ERR_IO: return "io error";
    case PJF_ERR_DISK_FULL: return "disk full";
    case PJF_ERR_NETWORK: return "network error";
    case PJF_ERR_HTTP_STATUS: return "http status error";
    case PJF_ERR_RANGE_UNSUPPORTED: return "range unsupported";
    case PJF_ERR_CANCELED: return "canceled";
    case PJF_ERR_TOO_MANY_RETRIES: return "too many retries";
    case PJF_ERR_TIMEOUT: return "timeout";
    case PJF_ERR_INTERNAL: return "internal error";
    }
    return "unknown";
}

} // namespace pjf
