#include "guard.h"
#include "state.h"
#include "timer.h"

#include <map>
#include <unordered_map>
#include <optional>
#include <thread>


namespace parallel {


class Timer::DelayedTask : public std::enable_shared_from_this<DelayedTask> {

    struct State : guard::EnableConditionNotification {
        using Request = state::Request<State>;

        State(DelayedTask &task)
            : _task(task)
        {}
        virtual ~State() = default;
        virtual Request run() {
            return {};
        }
        virtual Request cancel(state::StateChanged<State>) {
            return {};
        }
        virtual Request done() {
            return {};
        }
        virtual Request exception(std::exception_ptr) {
            return {};
        }
        virtual Request restart() {
            return {};
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

    protected:
        DelayedTask &task() {
            return _task;
        }

    private:
        DelayedTask &_task;
    };

public:
    DelayedTask(std::chrono::milliseconds delay, Task task, Timer &timer)
        : _state(state::Init<StateWaiting>(), *this)
        , _delay(delay)
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
        return _state.call<&State::cancel>();
    }

    bool restart() {
        return _state.call<&State::restart>();
    }

    void run() {
        if (_state.call<&State::run>()) {
            try {
                _task();
                _state.call<&State::done>();
            } catch (...) {
                _state.call<&State::exception>(std::current_exception());
            }
            _state.notifyAll();
        }
    }

    bool isWaiting() const {
        return _state.call<&State::isWaiting>();
    }
    bool isDone() const {
        return _state.call<&State::isDone>();
    }
    bool isRunning() const {
        return _state.call<&State::isRunning>();
    }
    bool isCancelled() const {
        return _state.call<&State::isCancelled>();
    }

private:
    struct StateWaiting : State {
        using State::State;
        Request run() override {
            return std::make_shared<StateRunning>(task());
        }
        Request cancel(state::StateChanged<State>) override {
            return std::make_shared<StateCancelled>(task());
        }
        Request restart() override {
            task().reschedule();
            return true;
        }

        bool isWaiting() const override {
            return true;
        }
    };

    struct StateRunning : State {
        StateRunning(DelayedTask &task)
            : State(task)
            , _executor(std::this_thread::get_id())
            , _restartWanted(false)
        {}

        Request cancel(state::StateChanged<State> changed) override {
            if (_executor == std::this_thread::get_id()) {
                return std::make_shared<StateCancelled>(task());
            }
            waitForNotification(changed);
            return {};
        }

        Request done() override {
            if (_restartWanted) {
                if (task().start())
                    return std::make_shared<StateWaiting>(task());
                return {};
            }
            return std::make_shared<StateDone>(task());
        }

        Request exception(std::exception_ptr ptr) override {
            return std::make_shared<StateException>(task(), ptr);
        }

        Request restart() override {
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
        using State::State;

        Request cancel(state::StateChanged<State>) override {
            return std::make_shared<StateCancelled>(task());
        }

        bool isDone() const override {
            return true;
        }
    };

    struct StateException : State {
        StateException(DelayedTask &task, std::exception_ptr ptr)
            : State(task)
            , _ptr(ptr)
        {}

        Request cancel(state::StateChanged<State>) override {
            std::rethrow_exception(_ptr);
        }
        Request restart() override {
            std::rethrow_exception(_ptr);
        }

        bool isDone() const override {
            std::rethrow_exception(_ptr);
        }

    private:
        std::exception_ptr _ptr;
    };

    struct StateCancelled : State {
        using State::State;

        bool isCancelled() const override {
            return true;
        }
    };

    bool reschedule() {
        return _timer->reschedule(shared_from_this());
    }

    bool start() {
        return _timer->start(shared_from_this());
    }

    state::State<State> _state;
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