#ifndef PARALLEL_DEFER_H
#define PARALLEL_DEFER_H

namespace parallel {

template<typename Callback>
struct Defer {
    Defer() = default;
    Defer(const Defer &) = delete;
    Defer(Defer &&other)
        : callback(std::move(other.callback))
        , called(other.called)
    {
        other.pass();
    }

    Defer(Callback callback)
        : callback(std::move(callback))
        , called(false)
    {}

    ~Defer() {
        run();
    }

    Defer &operator=(Defer other) {
        pass();
        swap(other);
        return *this;
    }

    Defer &operator=(Callback c) {
        callback = std::move(c);
        called = false;
        return *this;
    }

    void run() {
        if (!called) {
            callback();
            pass();
        }
    }

    void pass() {
        called = true;
    }

    bool passed() const {
        return called;
    }

    void swap(Defer &other) {
        using namespace std;
        swap(callback, other.callback);
        swap(called, other.called);
    }

private:
    Callback callback;
    bool called;
};

template<typename Callback>
Defer<Callback> makeDefer(Callback callback) {
    return {std::move(callback)};
}

} // namespace parallel

#endif