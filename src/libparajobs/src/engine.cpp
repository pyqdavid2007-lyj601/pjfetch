// libparajobs 引擎实现：每个 pjb_engine 一个私有 co::Scheduler，平台差异全经 libgo 头间接处理。
#include "internal.hpp"

using pjb::Engine;
using pjb::JobPtr;
using pjb::JobState;
using pjb::kInfiniteMs;

namespace {

// pjb_engine_t 是不完整类型，与 Engine* 无继承关系，句柄边界必须显式 reinterpret_cast。
inline Engine* as_engine(pjb_engine_t h)
{
    return reinterpret_cast<Engine*>(h);
}

inline pjb_engine_t as_handle(Engine* e)
{
    return reinterpret_cast<pjb_engine_t>(e);
}

// 活性握手：跑一个空任务并等它被执行完，确认调度线程已就绪。
// 必须如此：libgo 的 Stop() 紧跟 goStart()（调度线程尚未起来）会损坏调度器状态、进程退出时崩溃；
// 空任务跑完是可观察的就绪证明，比 sleep 可靠。任务体持 shared_ptr，超时后不会用到已释放对象。
inline void ensure_scheduler_live(Engine* e)
{
    struct Flag {
        std::mutex m;
        std::condition_variable cv;
        bool ran = false;
    };
    auto f = std::make_shared<Flag>();
    e->sched->CreateTask([f] {
        {
            std::lock_guard<std::mutex> lk(f->m);
            f->ran = true;
        }
        f->cv.notify_all();
    }, co::TaskOpt{});
    std::unique_lock<std::mutex> lk(f->m);
    f->cv.wait_for(lk, std::chrono::seconds(5), [&f] { return f->ran; });
}

// 任务收尾：置 done -> 唤醒等待者 -> on_done -> 未完成计数减一。每个 JobState 恰好被一个
// 协程体执行一次，故 on_done 必达恰一次（取消未启动 / 入队失败 / fn 抛异常同此）。pending
// 刻意在 on_done 之后递减，使 drain 返回时回调必已送达；故 on_done 内不得 drain（死锁）。
void complete_job(Engine* e, JobState& st)
{
    {
        std::lock_guard<std::mutex> lk(st.done_mtx);
        st.done.store(true, std::memory_order_release);
    }
    st.done_cv.notify_all();
    if (st.on_done)
        st.on_done(st.id, st.done_ud);
    {
        std::lock_guard<std::mutex> lk(e->pend_mtx);
        if (e->pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
            e->pend_cv.notify_all();
    }
}

void run_job(Engine* e, JobPtr st)
{
    if (!st->canceled.load(std::memory_order_relaxed)) {
        try {
            st->fn(st->ud);
        } catch (...) {
            // C ABI 约定用户回调异常不外溢到 libgo 调度器；任务照常按完成收尾
        }
    }
    complete_job(e, *st);
}

JobPtr find_job(Engine* e, pjb_job_id id)
{
    std::lock_guard<std::mutex> lk(e->reg_mtx);
    auto it = e->jobs.find(id);
    return it != e->jobs.end() ? it->second : nullptr;
}

} // namespace

extern "C" {

PARAOBS_API pjb_engine_t pjb_engine_create(int32_t min_threads, int32_t max_threads)
{
    if (min_threads < 1) {
        const int hw = static_cast<int>(std::thread::hardware_concurrency());
        min_threads = hw > 0 ? hw : 1;
    }
    if (max_threads == 0 || max_threads < min_threads)
        max_threads = min_threads;

    Engine* e = new (std::nothrow) Engine;
    if (!e)
        return nullptr;
    e->max_threads = max_threads;
    try {
        // goStart 每引擎只调一次：libgo 的 Start() 会锁一把永不解锁的自旋锁、二次调用抛异常。
        e->sched = co::Scheduler::Create();
        e->sched->goStart(min_threads, max_threads);
    } catch (...) {
        delete e;
        return nullptr;
    }
    return as_handle(e);
}

PARAOBS_API void pjb_engine_destroy(pjb_engine_t engine)
{
    Engine* e = as_engine(engine);
    if (!e)
        return;
    // 契约：create/destroy 单线程调用。顺序：drain（等含 on_done 的全部完成）-> 活性握手
    // -> 摘除注册表 -> Stop（不可逆，此后本引擎不可再用）。
    pjb_engine_drain(engine, kInfiniteMs);
    ensure_scheduler_live(e);
    {
        std::lock_guard<std::mutex> lk(e->reg_mtx);
        e->destroyed.store(true, std::memory_order_release);
        e->jobs.clear();   // 仅释放注册表引用；在跑协程各持自身副本，不受影响
    }
    e->sched->Stop();
    delete e;   // 仅外壳；Scheduler 由 libgo atexit 统一 delete
}

PARAOBS_API pjb_status pjb_engine_drain(pjb_engine_t engine, uint32_t timeout_ms)
{
    Engine* e = as_engine(engine);
    if (!e)
        return PJB_ERR_NOT_INITIALIZED;
    std::unique_lock<std::mutex> lk(e->pend_mtx);
    bool ok;
    if (timeout_ms == kInfiniteMs) {
        e->pend_cv.wait(lk, [e] { return e->pending.load(std::memory_order_acquire) == 0; });
        ok = true;
    } else {
        ok = e->pend_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                 [e] { return e->pending.load(std::memory_order_acquire) == 0; });
    }
    return ok ? PJB_OK : PJB_ERR_TIMEOUT;
}

PARAOBS_API int32_t pjb_engine_concurrency(pjb_engine_t engine)
{
    Engine* e = as_engine(engine);
    return e ? e->max_threads : 0;
}

PARAOBS_API pjb_job_id pjb_submit(pjb_engine_t engine, pjb_job_fn fn, void* ud,
                                  pjb_job_done_cb on_done, void* done_ud)
{
    Engine* e = as_engine(engine);
    if (!e || !fn)
        return PJB_INVALID_JOB;

    pjb_job_id id = PJB_INVALID_JOB;
    JobPtr st;
    {
        std::lock_guard<std::mutex> lk(e->reg_mtx);
        if (e->destroyed.load(std::memory_order_relaxed))
            return PJB_INVALID_JOB;
        try {
            st = std::make_shared<JobState>();
        } catch (...) {
            return PJB_INVALID_JOB;
        }
        id = e->next_id++;
        st->id = id;
        st->fn = fn;
        st->ud = ud;
        st->on_done = on_done;
        st->done_ud = done_ud;
        e->jobs.emplace(id, st);
        // 先计数再入队：保证 drain 不会在任务真正完成（含 on_done）前放过
        e->pending.fetch_add(1, std::memory_order_relaxed);
    }
    // CreateTask 仅把协程入队（互斥保护），任意线程可调 —— 跨线程提交之所以安全，是因为
    // 注册与计数已在上面的 reg_mtx 内完成；调度器尚未启动时任务也会在 goStart 生效后执行。
    try {
        co::TaskOpt opt{};
        e->sched->CreateTask([e, st] { run_job(e, st); }, opt);
    } catch (...) {
        complete_job(e, *st);   // 入队失败按“立即完成”收尾，保住计数平衡与 on_done 必达
    }
    return id;
}

PARAOBS_API pjb_status pjb_job_cancel(pjb_engine_t engine, pjb_job_id job)
{
    Engine* e = as_engine(engine);
    if (!e)
        return PJB_ERR_NOT_INITIALIZED;
    JobPtr st = find_job(e, job);
    if (!st)
        return PJB_ERR_INVALID_ARG;
    st->canceled.store(true, std::memory_order_relaxed);   // 协作式：由任务体自查退出
    return PJB_OK;
}

PARAOBS_API int32_t pjb_job_canceled(pjb_engine_t engine, pjb_job_id job)
{
    Engine* e = as_engine(engine);
    if (!e)
        return -1;
    JobPtr st = find_job(e, job);
    return st ? (st->canceled.load(std::memory_order_relaxed) ? 1 : 0) : -1;
}

PARAOBS_API pjb_status pjb_job_wait(pjb_engine_t engine, pjb_job_id job, uint32_t timeout_ms)
{
    Engine* e = as_engine(engine);
    if (!e)
        return PJB_ERR_NOT_INITIALIZED;
    JobPtr st = find_job(e, job);
    if (!st)
        return PJB_ERR_INVALID_ARG;
    // 契约：仅限外部线程（协程内阻塞等待会占死调度线程）；done 在持锁下翻转，不会丢唤醒。
    std::unique_lock<std::mutex> lk(st->done_mtx);
    if (timeout_ms == kInfiniteMs) {
        st->done_cv.wait(lk, [p = st.get()] { return p->done.load(std::memory_order_acquire); });
        return PJB_OK;
    }
    const bool ok = st->done_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                         [p = st.get()] { return p->done.load(std::memory_order_acquire); });
    return ok ? PJB_OK : PJB_ERR_TIMEOUT;
}

PARAOBS_API int32_t pjb_job_is_done(pjb_engine_t engine, pjb_job_id job)
{
    Engine* e = as_engine(engine);
    if (!e)
        return -1;
    JobPtr st = find_job(e, job);
    return st ? (st->done.load(std::memory_order_acquire) ? 1 : 0) : -1;
}

PARAOBS_API void pjb_sleep_ms(pjb_engine_t engine, uint32_t ms)
{
    Engine* e = as_engine(engine);
    if (e && e->sched && e->sched->IsCoroutine()) {
        // 协程内：libgo co_sleep —— Windows 经 kernel32 Sleep 钩子挂 fiber；禁用钩子时退化为占线程真睡。
        co_sleep(ms);
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

PARAOBS_API const char* pjb_version(void)
{
    return "0.1.0";
}

} // extern "C"
