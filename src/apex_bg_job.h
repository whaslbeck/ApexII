#ifndef APEX_BG_JOB_H
#define APEX_BG_JOB_H

/* One-shot background job for the ImGui frontend: runs a closure on a worker
 * thread so slow work (HTTP round-trips to the emulator, loading and analysing a
 * second ROM) does not freeze the UI.  The UI calls finished() every frame and
 * consumes the job's results once it returns true.
 *
 * Concurrency contract: the closure must only touch data the UI thread leaves
 * alone until finished() has returned true (typically: inputs copied in by
 * value, outputs written to members the UI reads only after completion).  A long
 * job should poll cancelled() so shutdown does not wait for it. */

#include <atomic>
#include <functional>
#include <thread>

class BackgroundJob {
  public:
    BackgroundJob() = default;
    ~BackgroundJob() {
        cancel_.store(true);
        if (thread_.joinable())
            thread_.join();
    }

    BackgroundJob(const BackgroundJob &) = delete;
    BackgroundJob &operator=(const BackgroundJob &) = delete;

    bool running() const {
        return running_;
    }

    bool cancelled() const {
        return cancel_.load(std::memory_order_relaxed);
    }

    void cancel() {
        cancel_.store(true);
    }

    /* Block until the worker has exited (e.g. before freeing what it writes). */
    void wait() {
        if (thread_.joinable())
            thread_.join();
        running_ = false;
    }

    /* Launch fn on a worker.  Precondition: !running(). */
    void start(std::function<void()> fn) {
        if (running_)
            return;
        if (thread_.joinable())
            thread_.join();
        cancel_.store(false);
        done_.store(false, std::memory_order_relaxed);
        running_ = true;
        thread_ = std::thread([this, fn] {
            fn();
            done_.store(true, std::memory_order_release); /* publishes fn's writes */
        });
    }

    /* True exactly once, on the first call after the job has completed; the job's
     * results may be read from then on.  Safe to call every frame. */
    bool finished() {
        if (!running_ || !done_.load(std::memory_order_acquire))
            return false;
        thread_.join();
        running_ = false;
        return true;
    }

  private:
    std::thread thread_;
    std::atomic<bool> done_{false};
    std::atomic<bool> cancel_{false};
    bool running_ = false;
};

#endif /* APEX_BG_JOB_H */
