#ifndef PARALLEL_TIMER_H
#define PARALLEL_TIMER_H

#include <chrono>

#include "thread_pool.h"

namespace parallel {

struct Timer {

    using Task = ThreadPool::Task;
    using TimePoint = std::chrono::steady_clock::time_point;

    enum class State {
        WAITING,
        BUSY,
        FINISHED,
        CANCELLED,
        FAILED_TO_SCHEDULE,
        EXCEPTION,
    };

    struct DelayedTask;
    struct Queue;

    struct Handle {

        Handle(std::shared_ptr<DelayedTask> task)
            : _task(std::move(task))
        {}

        Handle(const Handle &) = delete;
        Handle(Handle &&other)
            : _task(std::move(other._task))
        {}

        Handle &operator=(Handle other) = delete;

        ~Handle() {
            cancel();
        }

        bool cancel();
        State getState() const;

    private:
        std::shared_ptr<DelayedTask> _task;
    };

    Timer(ThreadPool &threadPool, unsigned maxQueueSize);
    ~Timer();

    Handle addDelayedTask(std::chrono::milliseconds duration, Task task);

private:
    struct Self;
    std::unique_ptr<Self> _self;
};

} // namespace parallel

#endif