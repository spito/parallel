#ifndef PARALLEL_STATE_H
#define PARALLEL_STATE_H

#include <functional>
#include <memory>

#include "guard.h"

namespace parallel::state {

template<typename State>
struct Request {
    Request(bool result = false)
        : result(result)
        , newState()
    {}

    Request(std::shared_ptr<State> newState)
        : result(newState)
        , newState(std::move(newState))
    {}

    template<
        typename SpecificState,
        typename = std::enable_if_t<std::is_base_of_v<State, SpecificState>>
    >
    Request(std::shared_ptr<SpecificState> newState)
        : result(newState)
        , newState(std::move(newState))
    {}

    const bool result = false;
    std::shared_ptr<State> newState;
};

template<typename State>
struct StateChanged {
    StateChanged(const std::shared_ptr<State> &shared, const std::shared_ptr<State> &persistent)
        : _shared(shared)
        , _persistent(persistent)
    {}
    bool operator()() const {
        return _shared != _persistent;
    }

private:
    const std::shared_ptr<State> &_shared;
    const std::shared_ptr<State> &_persistent;
};

template<auto>
struct Switcher {
    static constexpr const bool valid = false;
};

struct BaseSwitcher {
    static constexpr const bool valid = true;

protected:
    template<typename State, typename Call>
    bool operator()(std::shared_ptr<State> &state, Call call) {
        auto persistent = state;
        auto [ result, newState ] = call(persistent);

        static_assert(std::is_same_v<bool, std::decay_t<decltype(result)>>);

        if (state != persistent)
            return false;

        if (result && newState)
            state.swap(newState);
        
        return result;

    }
};

template<
    typename State,
    typename... Args,
    Request<State>(State::*member)(Args...)
>
struct Switcher<member> : BaseSwitcher {
    Switcher(Args... args)
        : _args(args...)
    {}
    bool operator()(std::shared_ptr<State> &state) {
        return BaseSwitcher::operator()(state, [&](std::shared_ptr<State> &persistent) {
            return std::apply([&](auto &&... args) {
                    return (persistent.get()->*member)(args...);
                },
                _args);
        });
    }

private:
    std::tuple<Args...> _args;
};

template<
    typename State,
    typename... Args,
    Request<State>(State::*member)(StateChanged<State>, Args...)
>
struct Switcher<member> : BaseSwitcher {
    Switcher(Args... args)
        : _args(args...)
    {}
    bool operator()(std::shared_ptr<State> &state) {
        return BaseSwitcher::operator()(state, [&](std::shared_ptr<State> &persistent) {
            return std::apply([&](auto &&... args) {
                    return (persistent.get()->*member)({state, persistent}, args...);
                },
                _args);
        });
    }

private:
    std::tuple<Args...> _args;
};

template<typename State>
struct Init {};

template<typename BaseState>
struct State {

    template<typename ConcreteState, typename... Args>
    State(Init<ConcreteState>, Args &&... args)
        : _state(std::make_shared<ConcreteState>(std::forward<Args>(args)...))
    {}

    template<auto member, typename... Args>
    auto call(Args &&... args) {
        if constexpr (Switcher<member>::valid)
            return _state.lock(Switcher<member>(std::forward<Args>(args)...));
        else
            return (_state.lock()->*member)(std::forward<Args>(args)...);
    }

    template<auto member, typename... Args>
    auto call(Args &&... args) const {
        return (_state.lock()->*member)(std::forward<Args>(args)...);
    }

    void notifyOne() {
        _state.notifyOne();
    }

    void notifyAll() {
        _state.notifyAll();
    }

private:
    guard::Exclusive<std::shared_ptr<BaseState>> _state;
};

} // namespace parallel::state

#endif