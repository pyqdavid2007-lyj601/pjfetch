#ifndef PARAOBS_PARAJOBS_H
#define PARAOBS_PARAJOBS_H

/* libparajobs.dll — C ABI 契约：纯 C，仅 <stdint.h>/<stddef.h> + export.h，不泄漏 libgo 类型。
 * 线程约定：submit/cancel/canceled/is_done 可跨线程调用；wait/drain 仅限外部线程
 * （在协程内调用会占死调度线程）；create/destroy 由单线程串行调用。
 */

#include <stdint.h>
#include <stddef.h>

#include "parajobs/export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pjb_engine* pjb_engine_t;     /* 不透明；每引擎一个私有 co::Scheduler */
typedef uint64_t pjb_job_id;
#define PJB_INVALID_JOB ((pjb_job_id)0)
typedef enum pjb_status { PJB_OK=0, PJB_ERR_INVALID_ARG, PJB_ERR_NOT_INITIALIZED,
                          PJB_ERR_TIMEOUT, PJB_ERR_CANCELED, PJB_ERR_INTERNAL } pjb_status;
typedef void (*pjb_job_fn)(void* userdata);              /* 在调度线程上执行 */
typedef void (*pjb_job_done_cb)(pjb_job_id, void* ud);   /* 必定恰好回调一次（取消未启动也触发）；
                                                            回调内禁止对本引擎调用 pjb_engine_drain/destroy（会自等死锁） */

PARAOBS_API pjb_engine_t pjb_engine_create(int32_t min_threads, int32_t max_threads);
PARAOBS_API void         pjb_engine_destroy(pjb_engine_t);           /* 先等全部任务完成再 Stop，不可逆 */
PARAOBS_API pjb_status   pjb_engine_drain(pjb_engine_t, uint32_t timeout_ms);
PARAOBS_API int32_t      pjb_engine_concurrency(pjb_engine_t);
PARAOBS_API pjb_job_id   pjb_submit(pjb_engine_t, pjb_job_fn fn, void* ud, pjb_job_done_cb on_done, void* done_ud);
PARAOBS_API pjb_status   pjb_job_cancel(pjb_engine_t, pjb_job_id);   /* 协作式：置取消标志 */
PARAOBS_API int32_t      pjb_job_canceled(pjb_engine_t, pjb_job_id); /* 1/0/-1 */
PARAOBS_API pjb_status   pjb_job_wait(pjb_engine_t, pjb_job_id, uint32_t timeout_ms);
PARAOBS_API int32_t      pjb_job_is_done(pjb_engine_t, pjb_job_id);
PARAOBS_API void         pjb_sleep_ms(pjb_engine_t, uint32_t ms);    /* 协程内 co_sleep（Sleep 钩子挂 fiber），外部线程标准库睡眠 */
PARAOBS_API const char*  pjb_version(void);

/* 错误码约定（timeout_ms == 0xFFFFFFFF 表示无限等待）：
 *   engine == NULL  -> submit: PJB_INVALID_JOB；concurrency/is_done/canceled: 0/-1；
 *                      wait/cancel/drain: PJB_ERR_NOT_INITIALIZED（destroy 为 no-op）
 *   job id 不存在   -> wait/cancel: PJB_ERR_INVALID_ARG；is_done/canceled: -1
 *   wait 超时未完成 -> PJB_ERR_TIMEOUT */

#ifdef __cplusplus
}
#endif

#endif /* PARAOBS_PARAJOBS_H */
