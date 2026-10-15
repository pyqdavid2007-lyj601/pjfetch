#ifndef PJFETCH_PJFETCH_H
#define PJFETCH_PJFETCH_H

/* libpjfetch.dll — C ABI 契约（自包含）。
 * String 规则：const char* 入参深拷贝，出参走调用方缓冲区，库不返回堆分配字符串。 */

#include <stdint.h>
#include <stddef.h>

#include "pjfetch/export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pjf_downloader* pjf_downloader_t;
typedef struct pjf_task* pjf_task_t;
typedef enum pjf_err { PJF_OK=0, PJF_ERR_INVALID_ARG, PJF_ERR_BAD_URL, PJF_ERR_IO,
    PJF_ERR_DISK_FULL, PJF_ERR_NETWORK, PJF_ERR_HTTP_STATUS, PJF_ERR_RANGE_UNSUPPORTED,
    PJF_ERR_CANCELED, PJF_ERR_TOO_MANY_RETRIES, PJF_ERR_TIMEOUT, PJF_ERR_INTERNAL } pjf_err;
typedef enum pjf_phase { PJF_PHASE_INIT=0, PJF_PHASE_PROBE, PJF_PHASE_DOWNLOAD, PJF_PHASE_FINALIZE, PJF_PHASE_DONE, PJF_PHASE_ERROR } pjf_phase;

/* pjf_options.flags 位标志 */
#define PJF_FLAG_VERIFY_SSL 0x1u  /* 置位 = 校验 TLS 证书（默认语义为校验） */
#define PJF_FLAG_VERBOSE    0x2u  /* libcurl verbose 输出到 stderr */
#define PJF_FLAG_RESUME     0x4u  /* 咨询 <output>.pjmeta 断点续传 sidecar */
#define PJF_FLAG_KEEP_META  0x8u  /* 成功后保留 sidecar（调试用） */

typedef struct pjf_options {
    const char* url; const char* output_path;      /* NULL→由 URL 推导 */
    int32_t N_segments;            /* 0=>8, 钳到 1..64 */
    int32_t max_retry;           /* 0=>5 */
    int32_t connect_timeout_ms;  /* 0=>15000 */
    int32_t timeout_ms;          /* 0=>不限 */
    int32_t low_speed_limit;     /* 0=>1024 */  int32_t low_speed_secs; /* 0=>20 */
    int64_t min_segment_bytes;   /* 0=>1MiB；文件 < 2x → 单段 */
    const char* user_agent; const char* referer; const char* proxy; /* NULL→env 默认, ""→禁代理 */
    const char* headers;         /* "K: V\r\nK2: V2" 或 NULL */
    uint32_t flags;
    int32_t engine_min_threads;  /* 0=>segments */  int32_t engine_max_threads; /* 0=>2*segments */
} pjf_options;

typedef struct pjf_progress { int64_t total_bytes /*-1 未知*/, downloaded_bytes;
    double bytes_per_sec, eta_seconds; int32_t active_segments, total_segments,
    http_status, using_range; } pjf_progress;
typedef struct pjf_segment { int32_t index; int64_t start, end, done;
    int32_t attempts, finished; } pjf_segment;
typedef struct pjf_result { pjf_err err; char message[512]; int64_t bytes;
    int32_t http_status; double seconds; int32_t resumed, range_used, segments_used; } pjf_result;

/* 回调只在单一 sampler 协程线程触发（~4Hz，不并发），内部可调 pjf_query_* 与 cancel。
 * sampler 只在实际下载阶段运行：探测/预分配/提交分段阶段的失败（URL 不可达、404、
 * 无 Range 等）不会触发任何回调，必须经 pjf_download_wait + pjf_download_result 获知。 */
typedef void (*pjf_progress_cb)(const pjf_progress*, void* ud);
typedef void (*pjf_status_cb)(int32_t phase, pjf_err last_err, int32_t http_status, void* ud);

PJFETCH_API pjf_downloader_t pjf_downloader_create(const pjf_options*);  /* 深拷贝 opts；坏参返回 NULL */
PJFETCH_API void             pjf_downloader_destroy(pjf_downloader_t);   /* 等全部任务结束后销毁 */
PJFETCH_API pjf_task_t  pjf_download_start(pjf_downloader_t, const char* out_override,
                               pjf_progress_cb, void*, pjf_status_cb, void*);
/* timeout_ms == 0 表示无限等待（阻塞到任务终态）；非 0 为等待上限，超时返回 PJF_ERR_TIMEOUT */
PJFETCH_API pjf_err     pjf_download_wait(pjf_task_t, uint32_t timeout_ms);
PJFETCH_API void        pjf_download_cancel(pjf_task_t);        /* 任意线程，含 Ctrl+C 处理器 */
PJFETCH_API int32_t     pjf_download_is_running(pjf_task_t);    /* 1 运行中 / 0 非运行 / -1 无效句柄 */
/* 返回 PJF_OK 表示"结果已成功填入 out"，下载本身的成败在 out->err 中；
 * 句柄无效返回 PJF_ERR_INVALID_ARG */
PJFETCH_API pjf_err     pjf_download_result(pjf_task_t, pjf_result* out);   /* wait 成功后有效 */
PJFETCH_API pjf_err     pjf_query_progress(pjf_task_t, pjf_progress*);      /* 轮询替代回调 */
PJFETCH_API int32_t     pjf_query_segments(pjf_task_t, pjf_segment* buf, int32_t cap);
PJFETCH_API int32_t     pjf_get_header(pjf_task_t, const char* name, char* buf, int32_t cap); /* Content-Disposition 用 */
/* 返回消息的完整长度（snprintf 语义，可能 >= cap）；缓冲区始终以 '\0' 结尾且不越界 */
PJFETCH_API int32_t     pjf_last_error(pjf_task_t, char* buf, int32_t cap);
/* 阻塞便捷版：内部 = start + wait(0) + result；返回值同 pjf_download_result 语义
 * （PJF_OK 表示 out 已填充，下载成败见 out->err） */
PJFETCH_API pjf_err     pjf_download(pjf_downloader_t, const pjf_options*, pjf_progress_cb, void*, pjf_result*);
PJFETCH_API const char* pjf_version(void);

#ifdef __cplusplus
}
#endif

#endif /* PJFETCH_PJFETCH_H */
