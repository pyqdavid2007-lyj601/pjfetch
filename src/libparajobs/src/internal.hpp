#ifndef PARAOBS_INTERNAL_HPP
#define PARAOBS_INTERNAL_HPP

// libparajobs 私有封装层：pjb_engine 对 libgo co::Scheduler 的一对一包装。本文件与
// engine.cpp 是唯二包含 libgo 头的 TU；平台面全经 libgo 头间接引入，不裸调平台 API。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>

#include "parajobs/parajobs.h"

// libgo 头必须放在标准头之后：coroutine.h 会 #define __const__ 等保留宏，其后的标准库头会被破坏。
#include "coroutine.h"
#include "scheduler/scheduler.h"

namespace pjb {

inline constexpr uint32_t kInfiniteMs = 0xFFFFFFFFu;

// 单个任务的共享状态：注册表与协程体各持一份 shared_ptr。
struct JobState
{
    pjb_job_id id = 0;

    // 提交时一次性写入（注册表锁内），之后协程侧只读
    pjb_job_fn fn = nullptr;
    void* ud = nullptr;
    pjb_job_done_cb on_done = nullptr;
    void* done_ud = nullptr;

    std::atomic<bool> canceled{false};   // 协作式取消标志，任务体自查退出
    std::atomic<bool> done{false};
    std::mutex done_mtx;
    std::condition_variable done_cv;
};

using JobPtr = std::shared_ptr<JobState>;

struct Engine
{
    // Scheduler 所有权在 libgo：Create() 已把 delete 登记进进程级 ExitList（atexit 统一
    // 释放），且 ~Scheduler 会置全局 IsExiting 标志 —— 本层因此绝不 delete 该指针。
    co::Scheduler* sched = nullptr;
    int32_t max_threads = 0;             // pjb_engine_concurrency 的返回值

    std::atomic<bool> destroyed{false};  // pjb_engine_destroy 之后置位

    std::mutex reg_mtx;
    std::unordered_map<pjb_job_id, JobPtr> jobs;
    pjb_job_id next_id = 1;              // reg_mtx 保护；0 保留为 PJB_INVALID_JOB

    // 未完成任务数。递减在 pend_mtx 内进行，保证 pjb_engine_drain 的条件变量不丢唤醒。
    std::atomic<int64_t> pending{0};
    std::mutex pend_mtx;
    std::condition_variable pend_cv;
};

} // namespace pjb

#endif // PARAOBS_INTERNAL_HPP
