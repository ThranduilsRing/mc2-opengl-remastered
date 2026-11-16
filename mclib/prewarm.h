#pragma once
#include "sim_overlap.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace mc2::prewarm {
// A main-thread sequence keeps its parser/files and pump meter alive across slices.
class Sequence {
public:
    struct promise_type {
        Sequence get_return_object() { return Sequence{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        std::suspend_always yield_value(int) noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() { error = std::current_exception(); }
        std::exception_ptr error;
    };
    explicit Sequence(std::coroutine_handle<promise_type> handle) : handle_(handle) {}
    Sequence(Sequence&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    ~Sequence() { if (handle_) handle_.destroy(); }
    bool step() {
        if (!handle_ || handle_.done()) return false;
        handle_.resume();
        if (handle_.promise().error) std::rethrow_exception(handle_.promise().error);
        return !handle_.done();
    }
private:
    std::coroutine_handle<promise_type> handle_;
};

struct State {
    std::function<bool()> step;
    std::function<std::function<void()>()> decode;
    std::function<void()> adopt;
    std::exception_ptr error;
    bool complete = false; // read/written only on Main
    double maxStepMs = 0;
};
using Job = std::shared_ptr<State>;

class Service {
public:
    Job submit(std::function<std::function<void()>()> decode) {
        overlap::assertMainThread("prewarm.submit");
        auto job = std::make_shared<State>();
        job->decode = std::move(decode);
        std::lock_guard lock(mutex_);
        if (stop_) throw std::logic_error("prewarm service is shut down");
        if (!worker_.joinable()) worker_ = std::thread([this] { work(); });
        pending_.push_back(job);
        wake_.notify_all();
        return job;
    }
    Job submitMain(std::function<bool()> step) {
        overlap::assertMainThread("prewarm.submitMain");
        if (stop_) throw std::logic_error("prewarm service is shut down");
        auto job = std::make_shared<State>();
        job->step = std::move(step);
        main_.push_back(job);
        return job;
    }
    void pumpMain(double budgetMs) {
        overlap::assertMainThread("prewarm.pumpMain");
        const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double, std::milli>(budgetMs);
        do {
            Job result;
            {
                std::lock_guard lock(mutex_);
                if (!results_.empty()) { result = results_.front(); results_.pop_front(); }
            }
            if (result) {
                result->complete = true;
                if (result->error) std::rethrow_exception(result->error);
                if (result->adopt) result->adopt();
                result->adopt = {};
            } else if (!main_.empty()) {
                auto job = main_.front(); main_.pop_front();
                const auto begin = std::chrono::steady_clock::now();
                bool more = false;
                try { more = job->step(); }
                catch (...) { job->complete = true; job->step = {}; throw; }
                job->maxStepMs = std::max(job->maxStepMs,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
                if (more) main_.push_back(job);
                else { job->step = {}; job->complete = true; }
            } else break;
        } while (std::chrono::steady_clock::now() < until);
    }
    void finish(const Job& job, const std::function<void()>& pump = {}) {
        while (job && !job->complete) {
            pumpMain(4.0);
            if (pump) pump();
            if (!job->complete) {
                std::unique_lock lock(mutex_);
                wake_.wait_for(lock, std::chrono::milliseconds(1), [this] { return !results_.empty(); });
            }
        }
    }
    void shutdown() {
        { std::lock_guard lock(mutex_); stop_ = true; }
        wake_.notify_all();
        if (worker_.joinable()) worker_.join();
        for (auto* queue : {&results_, &main_}) for (auto& job : *queue) {
            job->adopt = {}; job->step = {}; job->complete = true;
        }
        results_.clear(); main_.clear();
    }
    ~Service() { shutdown(); }
private:
    void work() {
        overlap::registerThread(overlap::Owner::Prewarm);
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] { return stop_ || !pending_.empty(); });
                if (pending_.empty()) return;
                job = pending_.front(); pending_.pop_front();
            }
            try { job->adopt = job->decode(); } catch (...) { job->error = std::current_exception(); }
            job->decode = {};
            { std::lock_guard lock(mutex_); results_.push_back(job); }
            wake_.notify_all();
        }
    }
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> pending_, results_, main_;
    std::thread worker_;
    bool stop_ = false;
};
inline Service& service() { static Service instance; return instance; }
inline void pumpMain(double budgetMs) { service().pumpMain(budgetMs); }
inline void finish(const Job& job, const std::function<void()>& pump = {}) { service().finish(job, pump); }
} // namespace mc2::prewarm
