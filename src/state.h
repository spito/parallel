#ifndef PARALLEL_STATE_H
#define PARALLEL_STATE_H

#include <functional>
#include <memory>

#include "guard.h"

namespace parallel::state {

template<typename State>
using Request = std::pair<bool, std::function<std::shared_ptr<State>()>>;

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
        auto [ result, ctor ] = call(persistent);

        static_assert(std::is_same_v<bool, decltype(result)>);

        if (state != persistent || !result)
            return false;
        if (auto newState = ctor())
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
    Request<State>(State::*member)(const std::shared_ptr<State> &, Args...)
>
struct Switcher<member> : BaseSwitcher {
    Switcher(Args... args)
        : _args(args...)
    {}
    bool operator()(std::shared_ptr<State> &state) {
        return BaseSwitcher::operator()(state, [&](std::shared_ptr<State> &persistent) {
            return std::apply([&](auto &&... args) {
                    return (persistent.get()->*member)(std::cref(state), args...);
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
            return (_state.lock().get()->*member)(std::forward<Args>(args)...);
    }

    template<auto member, typename... Args>
    auto call(Args &&... args) const {
        if constexpr (Switcher<member>::valid)
            return _state.lock(Switcher<member>(std::forward<Args>(args)...));
        else
            return (_state.lock().get()->*member)(std::forward<Args>(args)...);
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