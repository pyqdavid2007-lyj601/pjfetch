// pjb_smoke.cpp -- smoke tests for libparajobs (M1).
// Pure ASCII on purpose: this target compiles without /utf-8.
// Every failure prints a FAIL line and makes main return 1.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

#include "parajobs/parajobs.h"

namespace {

std::atomic<int> g_fail{0};

void check(bool cond, const char* what, int line)
{
    if (!cond) {
        std::printf("  FAIL(line %d): %s\n", line, what);
        g_fail.fetch_add(1);
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

using SteadyClock = std::chrono::steady_clock;

int64_t elapsed_ms(const SteadyClock::time_point& t0)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - t0).count();
}

void nap_ms(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// on_done fires right after done inside the same coroutine, so callers of
// pjb_job_wait must tolerate a short lag before the callback counter moves.
bool wait_until(const std::atomic<int>& v, int want)
{
    for (int i = 0; i < 200; ++i) {
        if (v.load(std::memory_order_acquire) >= want)
            return true;
        nap_ms(10);
    }
    return v.load(std::memory_order_acquire) >= want;
}

// ---- job bodies ---------------------------------------------------------

struct CountCtx
{
    std::atomic<int>* counter;
    std::atomic<int>* cb;
};

void counting_job(void* ud)
{
    CountCtx* c = static_cast<CountCtx*>(ud);
    c->counter->fetch_add(1, std::memory_order_relaxed);
}

void counting_done(pjb_job_id /*id*/, void* ud)
{
    CountCtx* c = static_cast<CountCtx*>(ud);
    c->cb->fetch_add(1, std::memory_order_relaxed);
}

void atomic_done(pjb_job_id /*id*/, void* ud)
{
    static_cast<std::atomic<int>*>(ud)->fetch_add(1, std::memory_order_relaxed);
}

// Cancelable sleeper: checks its own cancel flag once per 50ms tick; the job
// id is only known after pjb_submit returns, hence the id holder.
struct SleeperCtx
{
    pjb_engine_t engine;
    std::atomic<unsigned long long>* id_holder;
    std::atomic<int>* ran_full;   // set only if the 2s loop was never canceled
};

void sleeper_job(void* ud)
{
    SleeperCtx* c = static_cast<SleeperCtx*>(ud);
    for (int i = 0; i < 40; ++i) {   // 40 * 50ms = 2s max
        const unsigned long long id = c->id_holder->load(std::memory_order_relaxed);
        if (id != 0 && pjb_job_canceled(c->engine, static_cast<pjb_job_id>(id)) == 1)
            return;
        pjb_sleep_ms(c->engine, 50);
    }
    c->ran_full->fetch_add(1, std::memory_order_relaxed);
}

// Gate spinners: block their scheduler threads for real until released, so a queued job
// provably never starts. Must use std::condition_variable, not sleep_for -- with libgo
// hooks installed, Sleep inside a coroutine yields the fiber instead of blocking the thread.
struct SpinnerCtx
{
    std::atomic<int>* gate;
    std::condition_variable* cv;
};

void spinner_job(void* ud)
{
    SpinnerCtx* c = static_cast<SpinnerCtx*>(ud);
    std::mutex m;
    std::unique_lock<std::mutex> lk(m);
    c->cv->wait(lk, [&] { return c->gate->load(std::memory_order_acquire) != 0; });
}

struct TargetCtx
{
    std::atomic<int>* started;
};

void target_job(void* ud)
{
    static_cast<TargetCtx*>(ud)->started->fetch_add(1, std::memory_order_relaxed);
}

struct SleepCtx
{
    pjb_engine_t engine;
};

void sleep_300_job(void* ud)
{
    pjb_sleep_ms(static_cast<SleepCtx*>(ud)->engine, 300);
}

// ---- tests ---------------------------------------------------------------

void test_basic_submit_wait()
{
    std::printf("[1] create(2,4), 4 counting jobs, wait, NULL-arg safety\n");
    pjb_engine_t e = pjb_engine_create(2, 4);
    CHECK(e != nullptr);
    CHECK(pjb_engine_concurrency(e) == 4);

    std::atomic<int> counter{0};
    std::atomic<int> cb{0};
    CountCtx ctx{&counter, &cb};

    pjb_job_id ids[4];
    for (int i = 0; i < 4; ++i) {
        ids[i] = pjb_submit(e, counting_job, &ctx, counting_done, &ctx);
        CHECK(ids[i] != PJB_INVALID_JOB);
    }
    for (int i = 0; i < 4; ++i)
        CHECK(pjb_job_wait(e, ids[i], 10000) == PJB_OK);
    CHECK(counter.load() == 4);
    CHECK(wait_until(cb, 4));
    CHECK(cb.load() == 4);
    for (int i = 0; i < 4; ++i) {
        CHECK(pjb_job_is_done(e, ids[i]) == 1);
        CHECK(pjb_job_canceled(e, ids[i]) == 0);
    }

    // unknown job id
    CHECK(pjb_job_is_done(e, 999999) == -1);
    CHECK(pjb_job_canceled(e, 999999) == -1);
    CHECK(pjb_job_cancel(e, 999999) == PJB_ERR_INVALID_ARG);
    CHECK(pjb_job_wait(e, 999999, 0) == PJB_ERR_INVALID_ARG);

    // NULL fn
    CHECK(pjb_submit(e, nullptr, &ctx, nullptr, nullptr) == PJB_INVALID_JOB);

    // NULL engine returns safe values everywhere
    CHECK(pjb_engine_concurrency(nullptr) == 0);
    CHECK(pjb_engine_drain(nullptr, 0) == PJB_ERR_NOT_INITIALIZED);
    CHECK(pjb_submit(nullptr, counting_job, &ctx, nullptr, nullptr) == PJB_INVALID_JOB);
    CHECK(pjb_job_wait(nullptr, 1, 0) == PJB_ERR_NOT_INITIALIZED);
    CHECK(pjb_job_cancel(nullptr, 1) == PJB_ERR_NOT_INITIALIZED);
    CHECK(pjb_job_canceled(nullptr, 1) == -1);
    CHECK(pjb_job_is_done(nullptr, 1) == -1);
    pjb_engine_destroy(nullptr);   // must be a no-op

    pjb_engine_destroy(e);
    CHECK(pjb_version() != nullptr && pjb_version()[0] != '\0');
}

void test_cross_thread_submit()
{
    std::printf("[2] submit + wait from a non-scheduler thread\n");
    pjb_engine_t e = pjb_engine_create(2, 4);
    CHECK(e != nullptr);

    std::atomic<int> counter{0};
    std::atomic<int> cb{0};
    CountCtx ctx{&counter, &cb};

    std::thread worker([&ctx, e] {
        for (int i = 0; i < 4; ++i) {
            const pjb_job_id id = pjb_submit(e, counting_job, &ctx, counting_done, &ctx);
            if (id != PJB_INVALID_JOB)
                pjb_job_wait(e, id, 10000);
        }
    });
    worker.join();

    CHECK(counter.load() == 4);
    CHECK(wait_until(cb, 4));
    pjb_engine_destroy(e);
}

void test_cancel_running()
{
    std::printf("[3] cancel a running sleeper; on_done exactly once\n");
    pjb_engine_t e = pjb_engine_create(1, 2);
    CHECK(e != nullptr);

    std::atomic<unsigned long long> id_holder{0};
    std::atomic<int> ran_full{0};
    std::atomic<int> cb{0};
    SleeperCtx ctx{e, &id_holder, &ran_full};

    const pjb_job_id id = pjb_submit(e, sleeper_job, &ctx, atomic_done, &cb);
    CHECK(id != PJB_INVALID_JOB);
    id_holder.store(static_cast<unsigned long long>(id));

    nap_ms(150);   // let it enter the 50ms sleep loop
    CHECK(pjb_job_is_done(e, id) == 0);

    const SteadyClock::time_point t0 = SteadyClock::now();
    CHECK(pjb_job_cancel(e, id) == PJB_OK);
    CHECK(pjb_job_canceled(e, id) == 1);
    CHECK(pjb_job_wait(e, id, 5000) == PJB_OK);
    const int64_t took = elapsed_ms(t0);

    CHECK(wait_until(cb, 1));    // on_done fired exactly once
    CHECK(cb.load() == 1);
    CHECK(ran_full.load() == 0); // interrupted, did not run the full 2s
    CHECK(took < 2000);          // cancel made wait return well before the natural end
    pjb_engine_destroy(e);
}

void test_cancel_before_start()
{
    std::printf("[4] cancel before start: fn skipped, on_done still fires\n");
    // min == max == 2: the two gate spinners block both scheduler threads and the
    // dispatcher cannot grow the pool, so the queued target provably never starts
    // until the gate opens -- which happens after cancel.
    pjb_engine_t e = pjb_engine_create(2, 2);
    CHECK(e != nullptr);

    std::atomic<int> gate{0};
    std::condition_variable gate_cv;
    SpinnerCtx spin{&gate, &gate_cv};
    std::atomic<int> started{0};
    std::atomic<int> cb{0};
    TargetCtx tgt{&started};

    CHECK(pjb_submit(e, spinner_job, &spin, nullptr, nullptr) != PJB_INVALID_JOB);
    CHECK(pjb_submit(e, spinner_job, &spin, nullptr, nullptr) != PJB_INVALID_JOB);
    const pjb_job_id tid = pjb_submit(e, target_job, &tgt, atomic_done, &cb);
    CHECK(tid != PJB_INVALID_JOB);

    nap_ms(150);
    CHECK(pjb_job_is_done(e, tid) == 0);   // still queued behind the spinners

    CHECK(pjb_job_cancel(e, tid) == PJB_OK);
    gate.store(1, std::memory_order_release);
    gate_cv.notify_all();                  // release the scheduler threads
    CHECK(pjb_job_wait(e, tid, 5000) == PJB_OK);
    CHECK(wait_until(cb, 1));              // on_done fired although fn never ran
    CHECK(started.load() == 0);

    pjb_engine_destroy(e);
}

void test_drain_destroy()
{
    std::printf("[5] drain, drain timeout, destroy\n");
    pjb_engine_t e = pjb_engine_create(2, 4);
    CHECK(e != nullptr);

    std::atomic<int> counter{0};
    std::atomic<int> cb{0};
    CountCtx ctx{&counter, &cb};
    for (int i = 0; i < 10; ++i)
        CHECK(pjb_submit(e, counting_job, &ctx, counting_done, &ctx) != PJB_INVALID_JOB);

    CHECK(pjb_engine_drain(e, 10000) == PJB_OK);
    CHECK(counter.load() == 10);
    CHECK(cb.load() == 10);

    SleepCtx slow{e};
    CHECK(pjb_submit(e, sleep_300_job, &slow, nullptr, nullptr) != PJB_INVALID_JOB);
    CHECK(pjb_engine_drain(e, 10) == PJB_ERR_TIMEOUT);
    CHECK(pjb_engine_drain(e, 5000) == PJB_OK);

    pjb_engine_destroy(e);   // must not crash
}

void test_dual_engines()
{
    std::printf("[6] two engines coexist\n");
    pjb_engine_t a = pjb_engine_create(1, 1);
    pjb_engine_t b = pjb_engine_create(1, 1);
    CHECK(a != nullptr);
    CHECK(b != nullptr);
    CHECK(pjb_engine_concurrency(a) == 1);
    CHECK(pjb_engine_concurrency(b) == 1);

    std::atomic<int> ca{0};
    std::atomic<int> cba{0};
    std::atomic<int> cb{0};
    std::atomic<int> cbb{0};
    CountCtx xa{&ca, &cba};
    CountCtx xb{&cb, &cbb};

    for (int i = 0; i < 4; ++i) {
        const pjb_job_id ia = pjb_submit(a, counting_job, &xa, counting_done, &xa);
        const pjb_job_id ib = pjb_submit(b, counting_job, &xb, counting_done, &xb);
        CHECK(ia != PJB_INVALID_JOB);
        CHECK(ib != PJB_INVALID_JOB);
        CHECK(pjb_job_wait(a, ia, 10000) == PJB_OK);
        CHECK(pjb_job_wait(b, ib, 10000) == PJB_OK);
    }
    CHECK(ca.load() == 4);
    CHECK(cb.load() == 4);
    CHECK(wait_until(cba, 4));
    CHECK(wait_until(cbb, 4));

    pjb_engine_destroy(a);
    pjb_engine_destroy(b);
}

void test_sleep_external()
{
    std::printf("[7] pjb_sleep_ms on an external thread really sleeps\n");
    pjb_engine_t e = pjb_engine_create(1, 1);
    CHECK(e != nullptr);

    const SteadyClock::time_point t0 = SteadyClock::now();
    pjb_sleep_ms(e, 150);   // main thread = external thread (not a coroutine)
    CHECK(elapsed_ms(t0) >= 150);

    const SteadyClock::time_point t1 = SteadyClock::now();
    pjb_sleep_ms(nullptr, 60);   // NULL engine stays safe
    CHECK(elapsed_ms(t1) >= 60);

    pjb_engine_destroy(e);
}

} // namespace

int main()
{
    std::printf("pjb_smoke: libparajobs %s\n", pjb_version());

    test_basic_submit_wait();
    test_cross_thread_submit();
    test_cancel_running();
    test_cancel_before_start();
    test_drain_destroy();
    test_dual_engines();
    test_sleep_external();

    if (g_fail.load() != 0) {
        std::printf("pjb_smoke: FAILED (%d check(s))\n", g_fail.load());
        return 1;
    }
    std::printf("pjb_smoke: all tests passed\n");
    return 0;
}
