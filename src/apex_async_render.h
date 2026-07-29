#ifndef APEX_ASYNC_RENDER_H
#define APEX_ASYNC_RENDER_H

/* Runs apex_project_render() on a worker thread so a large re-render does not
 * freeze the UI thread.
 *
 * Concurrency contract (the caller MUST uphold it):
 *   Between start() and the take() that follows poll()==true, NO other thread
 *   may touch `project`.  apex_project_render_detached() mutates the project
 *   (analysis, config sorts, lazy label-set sorts), so any concurrent project
 *   access is a data race.  It does NOT touch the project's render cache, so the
 *   UI MAY keep reading the previously rendered document (which the GUI does via
 *   a project-free "frozen" renderer while running() is true).  The result is a
 *   freshly allocated document the caller installs with
 *   apex_project_adopt_render_cache().
 */

#include <thread>
#include <atomic>

extern "C" {
#include "apex_project.h"
#include "apex_render.h"
}

class AsyncRenderer {
public:
    AsyncRenderer() = default;
    ~AsyncRenderer() { if (thread_.joinable()) thread_.join(); }

    AsyncRenderer(const AsyncRenderer &) = delete;
    AsyncRenderer &operator=(const AsyncRenderer &) = delete;

    bool running() const { return running_; }

    /* Launch a render.  Precondition: !running(). */
    void start(ApexProject *project, int emit_xrefs, int emit_explain) {
        if (running_) {
            return;
        }
        project_ = project;
        xrefs_   = emit_xrefs;
        explain_ = emit_explain;
        result_  = nullptr;
        done_.store(false, std::memory_order_relaxed);
        running_ = true;
        thread_ = std::thread([this] {
            ApexRenderedDocument *d =
                apex_project_render_detached(project_, xrefs_, explain_);
            result_ = d;
            done_.store(true, std::memory_order_release);  /* publishes result_ */
        });
    }

    /* True once the worker has finished; safe to call every frame. */
    bool poll() const { return running_ && done_.load(std::memory_order_acquire); }

    /* Join the worker and hand back the freshly rendered detached document (the
     * caller installs it via apex_project_adopt_render_cache).  Call once, after
     * poll() has returned true.  After this the object is idle again. */
    ApexRenderedDocument *take() {
        if (thread_.joinable()) {
            thread_.join();
        }
        running_ = false;
        return result_;  /* synchronized by done_ release/acquire + join */
    }

private:
    std::thread                 thread_;
    std::atomic<bool>           done_{false};
    ApexRenderedDocument       *result_ = nullptr;
    ApexProject                *project_ = nullptr;
    int                         xrefs_ = 0;
    int                         explain_ = 0;
    bool                        running_ = false;
};

#endif /* APEX_ASYNC_RENDER_H */
