// libparajobs.dll —— C ABI 黑盒单元测试：只经公共契约头调用，不触碰 libgo。

#include "CppUTest/TestHarness.h"

extern "C" {
#include "parajobs/parajobs.h"
}

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// 任务负载：计数 + 可选睡眠 + 自我取消检测
struct JobPayload {
    std::atomic<int> ran{0};
    std::atomic<int> done_cb{0};
    std::atomic<int> finished_fully{0};
    std::atomic<int> saw_cancel{0};
    int sleep_ms = 0;
    pjb_engine_t eng = nullptr;
    pjb_job_id id = PJB_INVALID_JOB;
};

void counting_fn(void* ud) {
    auto* p = static_cast<JobPayload*>(ud);
    p->ran.fetch_add(1);
    p->finished_fully.fetch_add(1);
}

void sleepy_fn(void* ud) {
    // 长任务：分片睡眠并轮询取消标志（协作式取消的规范用法）
    auto* p = static_cast<JobPayload*>(ud);
    p->ran.fetch_add(1);
    int waited = 0;
    while (waited < p->sleep_ms) {
        if (p->id != PJB_INVALID_JOB && pjb_job_canceled(p->eng, p->id) == 1) {
            p->saw_cancel.fetch_add(1);
            return;                       // 提前退出，不置 finished_fully
        }
        pjb_sleep_ms(p->eng, 10);
        waited += 10;
    }
    p->finished_fully.fetch_add(1);
}

void on_done_fn(pjb_job_id, void* ud) {
    static_cast<JobPayload*>(ud)->done_cb.fetch_add(1);
}

constexpr uint32_t kInfinite = 0xFFFFFFFFu;

} // namespace

TEST_GROUP(Parajobs) {
    pjb_engine_t eng = nullptr;

    void setup() override { eng = pjb_engine_create(2, 4); }
    void teardown() override {
        if (eng) {
            pjb_engine_destroy(eng);
            eng = nullptr;
        }
    }
};

// --- 引擎生命周期 ---

TEST(Parajobs, create_returns_engine_with_requested_concurrency) {
    CHECK(eng != nullptr);
    LONGS_EQUAL(4, pjb_engine_concurrency(eng));
}

TEST(Parajobs, create_with_single_thread_reports_one) {
    pjb_engine_t e = pjb_engine_create(1, 1);
    CHECK(e != nullptr);
    LONGS_EQUAL(1, pjb_engine_concurrency(e));
    pjb_engine_destroy(e);
}

TEST(Parajobs, version_is_non_empty) {
    const char* v = pjb_version();
    CHECK(v != nullptr);
    CHECK(strlen(v) > 0);
}

TEST(Parajobs, destroy_null_is_noop) {
    pjb_engine_destroy(nullptr);          // 不得崩溃
}

// --- 提交 / 完成语义 ---

TEST(Parajobs, submitted_job_runs_and_signals_done_exactly_once) {
    JobPayload p;
    pjb_job_id id = pjb_submit(eng, counting_fn, &p, on_done_fn, &p);
    CHECK(id != PJB_INVALID_JOB);
    LONGS_EQUAL(PJB_OK, pjb_job_wait(eng, id, kInfinite));
    LONGS_EQUAL(1, p.ran.load());
    LONGS_EQUAL(1, p.done_cb.load());
    LONGS_EQUAL(1, pjb_job_is_done(eng, id));
}

TEST(Parajobs, job_without_done_callback_is_accepted) {
    JobPayload p;
    pjb_job_id id = pjb_submit(eng, counting_fn, &p, nullptr, nullptr);
    CHECK(id != PJB_INVALID_JOB);
    LONGS_EQUAL(PJB_OK, pjb_job_wait(eng, id, kInfinite));
    LONGS_EQUAL(1, p.ran.load());
}

TEST(Parajobs, many_jobs_all_complete) {
    constexpr int kN = 100;
    std::vector<JobPayload> payloads(kN);
    std::vector<pjb_job_id> ids;
    for (int i = 0; i < kN; ++i) ids.push_back(pjb_submit(eng, counting_fn, &payloads[i], nullptr, nullptr));

    int total_ran = 0;
    for (auto id : ids) {
        LONGS_EQUAL(PJB_OK, pjb_job_wait(eng, id, kInfinite));
        CHECK(id != PJB_INVALID_JOB);
    }
    for (auto& p : payloads) total_ran += p.ran.load();
    LONGS_EQUAL(kN, total_ran);
    LONGS_EQUAL(PJB_OK, pjb_engine_drain(eng, kInfinite));
}

TEST(Parajobs, wait_on_unknown_job_id_is_invalid_arg) {
    LONGS_EQUAL(PJB_ERR_INVALID_ARG, pjb_job_wait(eng, (pjb_job_id)999999, 0));
}

TEST(Parajobs, is_done_and_canceled_on_unknown_id_return_minus_one) {
    LONGS_EQUAL(-1, pjb_job_is_done(eng, (pjb_job_id)999999));
    LONGS_EQUAL(-1, pjb_job_canceled(eng, (pjb_job_id)999999));
}

// --- 取消 ---

TEST(Parajobs, cancel_running_job_stops_it_and_fires_done_once) {
    JobPayload p;
    p.eng = eng;
    p.sleep_ms = 3000;
    pjb_job_id id = pjb_submit(eng, sleepy_fn, &p, on_done_fn, &p);
    p.id = id;                            // 供 fn 内部轮询取消

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    LONGS_EQUAL(PJB_OK, pjb_job_cancel(eng, id));
    LONGS_EQUAL(PJB_OK, pjb_job_wait(eng, id, 5000));

    LONGS_EQUAL(1, p.ran.load());
    LONGS_EQUAL(0, p.finished_fully.load());   // 未跑满 = 确实被中断
    LONGS_EQUAL(1, p.saw_cancel.load());
    LONGS_EQUAL(1, p.done_cb.load());          // on_done 恰一次
    LONGS_EQUAL(1, pjb_job_canceled(eng, id));
}

TEST(Parajobs, cancel_before_start_skips_fn_but_fires_done_once) {
    // 用两个门闩占死调度线程（max==min 不扩容），使目标任务排队未启动。
    // 必须用 condition_variable 真正阻塞 OS 线程：libgo 在 Windows 钩住了 kernel32 的
    // Sleep，协程内的 sleep 只会让出 fiber 而不占住调度线程，门闩会失效（初版即因此误判）。
    struct Gate {
        std::atomic<int> entered{0};
        std::mutex m;
        std::condition_variable cv;
        bool release = false;
    } gate;
    auto blocker = [](void* ud) {
        auto* g = static_cast<Gate*>(ud);
        g->entered.fetch_add(1);
        std::unique_lock<std::mutex> lk(g->m);
        g->cv.wait(lk, [g] { return g->release; });
    };

    pjb_engine_t e = pjb_engine_create(2, 2);
    CHECK(e != nullptr);
    for (int i = 0; i < 2; ++i) pjb_submit(e, blocker, &gate, nullptr, nullptr);
    for (int i = 0; i < 200 && gate.entered.load() < 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    LONGS_EQUAL(2, gate.entered.load());       // 两条线程都被占住

    JobPayload p;
    pjb_job_id id = pjb_submit(e, counting_fn, &p, on_done_fn, &p);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    LONGS_EQUAL(0, p.ran.load());              // 确认尚未启动

    pjb_job_cancel(e, id);
    {
        std::lock_guard<std::mutex> lk(gate.m);
        gate.release = true;
    }
    gate.cv.notify_all();
    LONGS_EQUAL(PJB_OK, pjb_job_wait(e, id, 5000));

    LONGS_EQUAL(0, p.ran.load());              // fn 未执行
    LONGS_EQUAL(1, p.done_cb.load());          // on_done 仍恰一次
    pjb_engine_destroy(e);
}

// --- 等待超时 / drain ---

TEST(Parajobs, wait_with_zero_timeout_reports_timeout_for_running_job) {
    JobPayload p;
    p.eng = eng;
    p.sleep_ms = 1500;
    pjb_job_id id = pjb_submit(eng, sleepy_fn, &p, nullptr, nullptr);
    p.id = id;

    LONGS_EQUAL(PJB_ERR_TIMEOUT, pjb_job_wait(eng, id, 0));
    LONGS_EQUAL(0, pjb_job_is_done(eng, id));
    LONGS_EQUAL(PJB_OK, pjb_job_wait(eng, id, 5000));
}

TEST(Parajobs, drain_timeout_then_success) {
    JobPayload p;
    p.eng = eng;
    p.sleep_ms = 400;
    pjb_submit(eng, sleepy_fn, &p, nullptr, nullptr);

    LONGS_EQUAL(PJB_ERR_TIMEOUT, pjb_engine_drain(eng, 10));
    LONGS_EQUAL(PJB_OK, pjb_engine_drain(eng, 5000));
}

// --- 并发提交（跨线程） ---

TEST(Parajobs, submit_from_multiple_threads_is_safe) {
    constexpr int kThreads = 4;
    constexpr int kPerThread = 25;
    std::vector<JobPayload> payloads(kThreads * kPerThread);
    std::vector<std::thread> ts;
    std::atomic<int> submit_failures{0};

    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                auto& p = payloads[t * kPerThread + i];
                if (pjb_submit(eng, counting_fn, &p, nullptr, nullptr) == PJB_INVALID_JOB)
                    submit_failures.fetch_add(1);
            }
        });
    }
    for (auto& th : ts) th.join();

    LONGS_EQUAL(0, submit_failures.load());
    LONGS_EQUAL(PJB_OK, pjb_engine_drain(eng, kInfinite));

    int total = 0;
    for (auto& p : payloads) total += p.ran.load();
    LONGS_EQUAL(kThreads * kPerThread, total);
}

// --- 多引擎共存 ---

TEST(Parajobs, two_engines_run_independently) {
    pjb_engine_t a = pjb_engine_create(1, 1);
    pjb_engine_t b = pjb_engine_create(1, 1);
    CHECK(a != nullptr);
    CHECK(b != nullptr);
    CHECK(a != b);

    JobPayload pa, pb;
    pjb_job_id ia = pjb_submit(a, counting_fn, &pa, nullptr, nullptr);
    pjb_job_id ib = pjb_submit(b, counting_fn, &pb, nullptr, nullptr);
    LONGS_EQUAL(PJB_OK, pjb_job_wait(a, ia, kInfinite));
    LONGS_EQUAL(PJB_OK, pjb_job_wait(b, ib, kInfinite));
    LONGS_EQUAL(1, pa.ran.load());
    LONGS_EQUAL(1, pb.ran.load());

    pjb_engine_destroy(a);
    LONGS_EQUAL(1, pb.ran.load());        // 销毁 a 不影响 b
    pjb_engine_destroy(b);
}

// --- 参数安全（NULL 引擎） ---

TEST(Parajobs, null_engine_calls_are_safe) {
    JobPayload p;
    LONGS_EQUAL(PJB_INVALID_JOB, pjb_submit(nullptr, counting_fn, &p, nullptr, nullptr));
    LONGS_EQUAL(0, pjb_engine_concurrency(nullptr));   // NULL 引擎 -> 0（非 -1）
    // NULL 引擎的状态类调用统一返回 NOT_INITIALIZED（与未知 job id 的 INVALID_ARG 区分）
    LONGS_EQUAL(PJB_ERR_NOT_INITIALIZED, pjb_job_wait(nullptr, (pjb_job_id)1, 0));
    LONGS_EQUAL(-1, pjb_job_is_done(nullptr, (pjb_job_id)1));
    LONGS_EQUAL(PJB_ERR_NOT_INITIALIZED, pjb_job_cancel(nullptr, (pjb_job_id)1));
    pjb_engine_drain(nullptr, 0);
    pjb_sleep_ms(nullptr, 1);
}

// --- 睡眠语义 ---

TEST(Parajobs, sleep_ms_on_external_thread_really_sleeps) {
    auto t0 = std::chrono::steady_clock::now();
    pjb_sleep_ms(eng, 120);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    CHECK(ms >= 100);
}
