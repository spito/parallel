#ifndef PARALLEL_THREAD_POOL_H
#define PARALLEL_THREAD_POOL_H

#include <functional>
#include <memory>

namespace parallel {

struct ThreadPool {

    using Task = std::function<void()>;
    using ExceptionHandler = void(std::exception_ptr) noexcept;

    virtual ~ThreadPool() = default;
    virtual bool addTask(Task) = 0;

    static std::unique_ptr<ThreadPool> createSimple(unsigned threadCount, ExceptionHandler *handler = nullptr);

protected:
    ThreadPool() = default;
};

} // namespace parallel

#endif