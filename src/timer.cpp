#include "guard.h"
#include "timer.h"

#include <map>
#include <unordered_map>
#include <optional>
#include <thread>


namespace parallel {

struct Timer::DelayedTask : std::enable_shared_from_this<DelayedTask> {

    DelayedTask(std::chrono::milliseconds delay, Task task, Timer &timer)
        : _delay(delay)
        , _task(std::move(task))
        , _timer(&timer)
    {}

    TimePoint dueTime() const {
        return std::chrono::steady_clock::now() + _delay;
    }

    std::chrono::milliseconds delay() const {
        return _delay;
    }

    bool cancel() {
        return state()->cancel(*this);
    }

    bool restart() {
        return state()->restart(*this);
    }

    void run() {
        auto s = state();
        if (s->run(*this)) {
            try {
                _task();
                s->done(*this);
            } catch (...) {
                s->exception(*this, std::current_exception());
            }
            _state.notifyAll();
        }
    }

    bool isWaiting() const {
        return state()->isWaiting();
    }
    bool isDone() const {
        return state()->isDone();
    }
    bool isRunning() const {
        return state()->isRunning();
    }
    bool isCancelled() const {
        return state()->isCancelled();
    }

private:
    struct State : guard::EnableConditionNotification {
        virtual ~State() = default;
        virtual bool run(DelayedTask &) {
            return false;
        }
        virtual bool cancel(DelayedTask &) {
            return false;
        }
        virtual bool done(DelayedTask &) {
            return false;
        }
        virtual bool exception(DelayedTask &, std::exception_ptr) {
            return false;
        }
        virtual bool restart(DelayedTask &task) {
            if (!task.exchangeState<StateWaiting>(this))
                return false;
            return task.start();
        }

        virtual bool isWaiting() const {
            return false;
        }
        virtual bool isDone() const {
            return false;
        }
        virtual bool isRunning() const {
            return false;
        }
        virtual bool isCancelled() const {
            return false;
        }
    };

    struct StateWaiting : State {
        bool run(DelayedTask &task) override {
            return task.exchangeState<StateRunning>(this);
        }
        bool cancel(DelayedTask &task) override {
            return task.exchangeState<StateCancelled>(this);
        }
        bool restart(DelayedTask &task) override {
            return task.reschedule();
        }

        bool isWaiting() const override {
            return true;
        }
    };

    struct StateRunning : State {
        StateRunning()
            : _executor(std::this_thread::get_id())
            , _restartWanted(false)
        {}
        bool cancel(DelayedTask &task) override {
            if (_executor != std::this_thread::get_id())
                waitForNotification([&] { return task.state().get() != this; });
            return false;
        }
        bool done(DelayedTask &task) override {
            if (!_restartWanted)
                return task.exchangeState<StateDone>(this);

            if (!task.exchangeState<StateWaiting>(this))
                return false;
            return task.restart();
        }
        bool exception(DelayedTask &task, std::exception_ptr ptr) override {
            return task.exchangeState<StateException>(this, ptr);
        }
        bool restart(DelayedTask &task) {
            _restartWanted = true;
            return true;
        }

        bool isRunning() const override {
            return true;
        }
    private:
        std::thread::id _executor;
        bool _restartWanted;
    };

    struct StateDone : State {
        bool cancel(DelayedTask &) override {
            return false;
        }

        bool isDone() const override {
            return true;
        }
    };

    struct StateException : State {
        StateException(std::exception_ptr ptr)
            : _ptr(ptr)
        {}
        bool restart(DelayedTask &) override {
            std::rethrow_exception(_ptr);
        }

        bool isDone() const override {
            std::rethrow_exception(_ptr);
        }

    private:
        std::exception_ptr _ptr;
    };

    struct StateCancelled : State {
        bool restart(DelayedTask &) override {
            return false;
        }
        bool isCancelled() const override {
            return true;
        }
    };

    template<typename NewState, typename... Args>
    bool exchangeState(const State *old, Args &&... args) {
        return _state.lock([&](auto &state) {
            if (state.get() != old)
                return false;
            state = std::make_shared<NewState>(std::forward<Args>(args)...);
            return true;
        });
    }
    std::shared_ptr<State> state() const {
        return *_state.lock();
    }

    bool reschedule() {
        return _timer->reschedule(shared_from_this());
    }

    bool start() {
        return _timer->start(shared_from_this());
    }

    guard::Exclusive<std::shared_ptr<State>> _state;
    const std::chrono::milliseconds _delay;
    const Task _task;
    Timer * const _timer;
};

struct Timer::Queue : guard::EnableConditionNotification {

    Queue(unsigned maxQueueSize)
        : _quit(false)
        , _maxQueueSize(maxQueueSize)
    {}

    void stop() {
        _quit = true;
        notifyOne();
    }

    std::shared_ptr<DelayedTask> getTask() {
        while (!_quit) {
            if (taskIsReady()) {
                auto task = std::move(_queue.begin()->second);
                _queue.erase(_queue.begin());
                _mapping.erase(task);
                return task;
            }

            if (auto point = nearestTask())
                waitForNotification(point.value(), [&]{ return _quit || taskIsReady(); });
            else
                waitForNotification([&] { return _quit || nearestTask(); });
        }
        return {};
    }

    bool addTask(std::shared_ptr<DelayedTask> task) {
        if (_quit || _maxQueueSize <= _queue.size())
            return false;
        auto dueTime = task->dueTime();
        if (!_mapping.emplace(task, dueTime).second)
            return false;

        placeTask(dueTime, std::move(task));
        return true;
    }

    bool rescheduleTask(std::shared_ptr<DelayedTask> task) {
        if (_quit)
            return false;

        auto map = _mapping.find(task);
        if (map == _mapping.end())
            return false;

        auto range = _queue.equal_range(map->second);
        for (auto i = range.first; i != range.second; ++i) {
            if (i->second == task) {
                _queue.erase(i);
                break;
            }
        }

        auto dueTime = task->dueTime();
        map->second = dueTime;
        placeTask(dueTime, std::move(task));
        return true;
    }

    void cancelAll() {
        for (auto &[_, task] : _queue) {
            task->cancel();
        }
    }

private:
    void placeTask(TimePoint dueTime, std::shared_ptr<DelayedTask> task) {
        auto position = _queue.emplace(dueTime, std::move(task));
        if (position == _queue.begin())
            notifyOne();

    }

    bool taskIsReady() {
        auto now = std::chrono::steady_clock::now();
        return nearestTask().value_or(TimePoint::max()) <= now;
    }

    std::optional<TimePoint> nearestTask() const {
        if (_queue.empty())
            return std::nullopt;
        return _queue.begin()->first;
    }

    bool _quit;
    unsigned _maxQueueSize;
    std::multimap<TimePoint, std::shared_ptr<Timer::DelayedTask>> _queue;
    std::unordered_map<std::shared_ptr<Timer::DelayedTask>, TimePoint> _mapping;
};

struct Timer::Self {

    Self(ThreadPool &threadPool, unsigned maxQueueSize)
        : _threadPool(threadPool)
        , _queue(maxQueueSize)
        , _dispatcher([this]{ dispatcher(); })
    {}

    ~Self() {
        _queue.lock()->stop();
        _dispatcher.join();
        _queue.lock()->cancelAll();
    }

    std::shared_ptr<DelayedTask> addTask(std::shared_ptr<DelayedTask> task) {
        if (!_queue.lock()->addTask(task))
            task->cancel();
        return task;
    }

    std::shared_ptr<DelayedTask> rescheduleTask(std::shared_ptr<DelayedTask> task) {
        if (!_queue.lock()->rescheduleTask(task))
            task->cancel();
        return task;
    }

private:
    void dispatcher() {
        while (auto task = _queue.lock()->getTask()) {
            if (!_threadPool.addTask([task, this]{ task->run(); }))
                task->cancel();
        }
    }

    ThreadPool &_threadPool;
    guard::Exclusive<Queue> _queue;
    std::thread _dispatcher;
};

bool Timer::Handle::cancel() {
    return _task->cancel();
}

bool Timer::Handle::isCancelled() const {
    return _task->isCancelled();
}

bool Timer::Handle::isDone() const {
    return _task->isDone();
}

bool Timer::Handle::isRunning() const {
    return _task->isRunning();
}

bool Timer::Handle::isWaiting() const {
    return _task->isWaiting();
}

std::chrono::milliseconds Timer::Handle::delay() const {
    return _task->delay();
}

Timer::Timer(ThreadPool &threadPool, unsigned maxQueueSize)
    : _self(std::make_unique<Self>(threadPool, maxQueueSize))
{}

Timer::~Timer() = default;

Timer::Handle Timer::addDelayedTask(std::chrono::milliseconds duration, Task task) {
    return _self->addTask(std::make_shared<DelayedTask>(duration, std::move(task), *this));
}

bool Timer::start(std::shared_ptr<DelayedTask> task) {
    _self->addTask(task);
    return !task->isCancelled();
}

bool Timer::reschedule(std::shared_ptr<DelayedTask> task) {
    _self->rescheduleTask(task);
    return !task->isCancelled();
}

} // namespace parallel