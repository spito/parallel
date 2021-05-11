#include <queue>
#include <thread>
#include <vector>
#include <iostream>

#include "guard.h"
#include "thread_pool.h"

namespace parallel {

struct TaskQueue : guard::EnableConditionNotification {

    using Task = ThreadPool::Task;

    TaskQueue()
        : _quit(false)
    {}

    bool addTask(Task task) {
        if (_quit || !task)
            return false;
        _queue.push(std::move(task));
        notifyOne();
        return true;
    }

    void stop() {
        _quit = true;
        notifyAll();
    }

    Task getTask() {
        while (!_quit) {
            waitForNotification([&] { return _quit || !_queue.empty(); });

            if (!_queue.empty()) {
                Task task = _queue.front();
                _queue.pop();
                return task;
            }
        }
        return {};
    }

private:
    bool _quit;
    std::queue<Task> _queue;
};

struct SimpleThreadPool : ThreadPool {

    SimpleThreadPool(unsigned threadCount, ExceptionHandler handler)
        : _handler(std::move(handler))
    {
        _workers.reserve(threadCount);
        for (unsigned i = 0; i != threadCount; ++i)
            _workers.emplace_back([this]{ run(); });
    }

    ~SimpleThreadPool() {
        _queue.lock()->stop();
        for (auto &thread : _workers)
            thread.join();
    }

    bool addTask(Task task) override {
        return _queue.lock()->addTask(std::move(task));
    }

private:
    void run() noexcept {
        while (Task task = _queue.lock()->getTask()) {
            try {
                task();
            } catch (...) {
                if (!_handler)
                    std::terminate();
                _handler(std::current_exception());
            }
        }
    }

    const ExceptionHandler _handler;
    std::vector<std::thread> _workers;
    guard::Exclusive<TaskQueue> _queue;
};


std::unique_ptr<ThreadPool> ThreadPool::createSimple(unsigned threadCount, ExceptionHandler handler) {
    return std::make_unique<SimpleThreadPool>(threadCount, std::move(handler));
}


} // namespace parallel

