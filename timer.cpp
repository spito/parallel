#include "guard.h"
#include "timer.h"

#include <map>
#include <optional>
#include <thread>


namespace parallel {

struct Timer::DelayedTask {

    DelayedTask(std::chrono::milliseconds delay, Task task)
        : _dueTime(std::chrono::steady_clock::now() + delay)
        , _task(std::move(task))
    {}

    TimePoint dueTime() const {
        return _dueTime;
    }

    State getState() const {
        return _state.accessTo().object().getState();
    }

    bool cancel() {
        return _state.accessTo().object().cancel();
    }

    void run() {
        if (_state.accessTo().object().canRun()) {
            try {
                _task();
                _state.accessTo().object().finished();
            } catch (...) {
                _state.accessTo().object().exception(std::current_exception());
            }
        }
    }

    void failedToSchedule() {
        _state.accessTo().object().failedToSchedule();
    }

private:
    struct StateKeeper : guard::EnableConditionNotification {

        StateKeeper()
            : _state(State::WAITING)
        {}

        State getState() const {
            if (_exception)
                std::rethrow_exception(_exception);

            return _state;
        }

        bool cancel() {
            if (_state == State::WAITING) {
                _state = State::CANCELLED;
                return true;
            }
            waitForNotification([&]{ return _state != State::BUSY; });
            return false;
        }

        bool canRun() {
            if (_state != State::WAITING)
                return false;
            _state = State::BUSY;
            return true;
        }

        void exception(std::exception_ptr ptr) {
            _state = State::EXCEPTION;
            _exception = ptr;
            notifyAll();
        }

        void finished() {
            _state = State::FINISHED;
            notifyAll();
        }

        void failedToSchedule() {
            _state = State::FAILED_TO_SCHEDULE;
        }

    private:
        State _state;
        std::exception_ptr _exception;
    };

    guard::Exclusive<StateKeeper> _state;
    const TimePoint _dueTime;
    const Task _task;
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
            if (auto point = nearestTask())
                waitForNotification(point.value(), [&]{ return _quit || taskIsReady(); });
            else
                waitForNotification([&] { return _quit || nearestTask(); });

            if (taskIsReady()) {
                auto task = _queue.begin()->second;
                _queue.erase(_queue.begin());
                return task;
            }
        }
        return {};
    }

    bool addTask(std::shared_ptr<DelayedTask> task) {
        if (_maxQueueSize <= _queue.size())
            return false;
        auto position = _queue.emplace(task->dueTime(), std::move(task));
        if (position == _queue.begin())
            notifyOne();
        return true;
    }

private:
    bool taskIsReady() {
        auto now = std::chrono::steady_clock::now();
        return nearestTask().value_or(now) < now;
    }

    std::optional<TimePoint> nearestTask() const {
        if (_queue.empty())
            return std::nullopt;
        return _queue.begin()->first;
    }

    bool _quit;
    unsigned _maxQueueSize;
    std::multimap<TimePoint, std::shared_ptr<Timer::DelayedTask>> _queue;
};

struct Timer::Self {

    Self(ThreadPool &threadPool, unsigned maxQueueSize)
        : _threadPool(threadPool)
        , _queue(maxQueueSize)
        , _dispatcher([this]{ dispatcher(); })
    {}

    ~Self() {
        _queue.accessTo().object().stop();
        _dispatcher.join();
    }

    Handle addTask(std::shared_ptr<DelayedTask> task) {
        if (!_queue.accessTo().object().addTask(task))
            task->failedToSchedule();
        return task;
    }

private:
    void dispatcher() {
        while (auto task = _queue.accessTo().object().getTask()) {
            if (!_threadPool.addTask([task, this]{ task->run(); }))
                task->failedToSchedule();
        }
    }

    ThreadPool &_threadPool;
    guard::Exclusive<Queue> _queue;
    std::thread _dispatcher;
};

bool Timer::Handle::cancel() {
    return _task->cancel();
}

Timer::State Timer::Handle::getState() const {
    return _task->getState();
}

Timer::Timer(ThreadPool &threadPool, unsigned maxQueueSize)
    : _self(std::make_unique<Self>(threadPool, maxQueueSize))
{}

Timer::~Timer() = default;

Timer::Handle Timer::addDelayedTask(std::chrono::milliseconds duration, Task task) {
    return _self->addTask(std::make_shared<DelayedTask>(duration, std::move(task)));
}

} // namespace parallel