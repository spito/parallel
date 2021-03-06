#ifndef PARALLEL_GUARD_H
#define PARALLEL_GUARD_H

#include <initializer_list>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <type_traits>

#include "defer.h"
#include "platform.h" 

/**
 * @author  "Jiri Weiser" <jiri.weiser@gmail.com>
 * @date    2021-01-13
 * 
 * Parallel guards are useful to protect data from being abused in concurrency run.
 * Guards with locks:
 *      parallel::guard::Exclusive   - standard (exclusive) mutex semantic
 *      parallel::guard::Shared      - shared (read-write) mutex semantic
 *      parallel::guard::Recursive   - recursive mutex semantic
 * 
 *      To access the guarded object, you need to chose one of the approaches:
 *          1) Use accessTo method to obtain proxy
 * 
 *              parallel::guard::Shared<Object> smth;
 *              smth.lock()->someMethod();
 * 
 *              auto proxy = smth.constLock();
 *              proxy->constMethod1();
 *              proxy->constMethod2();
 * 
 *          2) Pass a functor to the access method
 * 
 *              parallel::guard::Exclusive<Object> smth;
 *              smth.lock([&](auto &object) {
 *                  object.someMethod();
 *              });
 * 
 *      In case you need to lock more guarded objects at once and need to avoid deadlock, use lockAll function:
 * 
 *      auto result = parallel::guard::lockAll([](auto &object1, auto &object2) {
 *          return object1.someOperation() + object2.someOtherOperation();
 *      }, guardedObject1, guardedObject2);
 * 
 * 
 * 
 * 
 */

namespace parallel::guard {
namespace detail {

struct SignalableLock {
    virtual ~SignalableLock() = default;
    virtual void lock() = 0;
    virtual bool try_lock() = 0;
    virtual void unlock() = 0;
};

/**
 * @brief 
 * 
 */
class EnableConditionNotification {
protected:
    EnableConditionNotification()
        : _lock(nullptr)
        , _cv()
    {}

    template<typename Predicate>
    void waitForNotification(Predicate predicate) {
        if (!_lock)
            throw std::logic_error("cannot wait for signal");
        auto _ = makeDefer([&, lock = _lock]{ setSignalableLock(lock); });
        _cv.wait(*_lock, std::move(predicate));
    }

    template<typename Rep, typename Period, typename Predicate>
    bool waitForNotification(const std::chrono::duration<Rep, Period> &duration, Predicate predicate) {
        if (!_lock)
            throw std::logic_error("cannot wait for signal");
        auto _ = makeDefer([&, lock = _lock]{ setSignalableLock(lock); });
        return _cv.wait_until(*_lock, std::chrono::steady_clock::now() + duration, std::move(predicate));
    }

    template<typename Rep, typename Period, typename Predicate>
    bool waitForNotification(const std::chrono::time_point<Rep, Period> &timePoint, Predicate predicate) {
        if (!_lock)
            throw std::logic_error("cannot wait for signal");
        auto _ = makeDefer([&, lock = _lock]{ setSignalableLock(lock); });
        return _cv.wait_until(*_lock, timePoint, std::move(predicate));
    }

    void notifyOne() {
        _cv.notify_one();
    }

    void notifyAll() {
        _cv.notify_all();
    }

private:
    template<
        typename Object,
        typename Mutex,
        template<typename> class WriteLockTemplate,
        template<typename> class ReadLockTemplate,
        bool signalable
    >
    friend class Guard;

    void setSignalableLock(SignalableLock *lock) {
        _lock = lock;
    }

    SignalableLock * _lock;
    std::condition_variable_any _cv;
};

template<typename Object>
struct Traits {
    static constexpr const bool isNotifiable = std::is_base_of_v<EnableConditionNotification, std::remove_const_t<Object>>;
    using Handle = std::add_lvalue_reference_t<Object>;
    using Reference = std::add_lvalue_reference_t<Object>;
    using Pointer = std::add_pointer_t<Object>;
    using ConstReference = std::add_const_t<Reference>;
    using ConstPointer = std::add_const_t<Pointer>;

    static Handle move(Handle handle) {
        return handle;
    }

    template<typename H>
    static H &access(H &object) {
        return object;
    }
};

template<typename Object>
struct Traits<std::shared_ptr<Object>> {
    static constexpr const bool isNotifiable = std::is_base_of_v<EnableConditionNotification, Object>;
    using Handle = std::shared_ptr<Object>;
    using Reference = std::add_lvalue_reference_t<Object>;
    using Pointer = std::add_pointer_t<Object>;
    using ConstReference = std::add_const_t<Reference>;
    using ConstPointer = std::add_const_t<Pointer>;

    static Handle &&move(Handle &handle) {
        return std::move(handle);
    }
    static Handle &&move(Handle &&handle) {
        return std::move(handle);
    }

    static Reference access(Handle &object) {
        return *object;
    }
    static ConstReference access(const Handle &object) {
        return *object;
    }
};

template<typename Object>
struct Traits<const std::shared_ptr<Object>> {
    static constexpr const bool isNotifiable = std::is_base_of_v<EnableConditionNotification, Object>;
    using Handle = std::shared_ptr<Object>;
    using Reference = std::add_lvalue_reference_t<std::add_const_t<Object>>;
    using Pointer = std::add_pointer_t<std::add_const_t<Object>>;
    using ConstReference = Reference;
    using ConstPointer = Pointer;

    static Handle &&move(Handle &handle) {
        return std::move(handle);
    }
    static Handle &&move(Handle &&handle) {
        return std::move(handle);
    }

    static Reference access(Handle &object) {
        return *object;
    }
    static Reference access(const Handle &object) {
        return *object;
    }
};

template<typename Object>
constexpr const bool isNotifiable = Traits<Object>::isNotifiable;
/**
 * @brief Proxy to the object. The Proxy is responsible for the locking and unlocking
 *      of the mutex using @p Lock strategy.
 * 
 * @tparam ObjectReference Reference to the wrapped object
 * @tparam Lock Mutex ownership RAII wrapper
 */
template<
    typename Object,
    typename Lock
>
struct Proxy {

    using Handle = typename Traits<Object>::Handle;
    using Reference = typename Traits<Object>::Reference;
    using ConstReference = typename Traits<Object>::ConstReference;
    using Pointer = typename Traits<Object>::Pointer;
    using ConstPointer = typename Traits<Object>::ConstPointer;

    /**
     * @brief Construct a new Proxy object
     * 
     * @param ref A reference to the guarded object
     * @param lock A RAII wrapper
     */
    Proxy(Handle handle, Lock &&lock)
        : _handle(Traits<Object>::move(handle))
        , _lock(std::move(lock))
    {}

    Proxy(const Proxy &) = delete;
    Proxy(Proxy &&other)
        : _handle(Traits<Object>::move(other._handle))
        , _lock(std::move(other._lock))
    {}

    Proxy &operator=(Proxy) = delete;

    Reference get() {
        return Traits<Object>::access(_handle);
    }

    ConstReference get() const {
        return Traits<Object>::access(_handle);
    }

    Reference operator*() {
        return Traits<Object>::access(_handle);
    }

    ConstReference operator*() const {
        return Traits<Object>::access(_handle);
    }

    /**
     * @brief Access to the guarded object.
     * 
     * @return ObjectRef
     */
    Pointer operator->() {
        return &Traits<Object>::access(_handle);
    }

    /**
     * @brief Constant access to the guarded object.
     * 
     * @return std::add_const_t<ObjectReference> 
     */
    ConstPointer operator->() const {
        return &Traits<Object>::access(_handle);
    }

    template<typename Member>
    auto operator->*(Member member) const {
        return [=](auto &&... args) {
            return ((**this).*member)(std::forward<decltype(args)>(args)...);
        };
    }

private:
    Handle _handle;
    Lock _lock;
};

/**
 * @brief Guard for any object in parallel environment.
 * 
 * @tparam Object Object to be guarded.
 * @tparam Mutex Type of mutex to be used.
 * @tparam WriteLockTemplate Write/exclusive lock guard template (i.e. std::lock_guard).
 * @tparam ReadLockTemplate Read/shared lock guard template (applicable only to const access to the object).
 *      Defaults to @p WriteLockTemplate if not present.
 * @tparam notifiable 
 */
template<
    typename Object,
    typename Mutex,
    template<typename> class WriteLockTemplate,
    template<typename> class ReadLockTemplate = WriteLockTemplate,
    bool notifiable = isNotifiable<Object>
>
struct alignas(hardware_destructive_interference_size) Guard {

    template<typename F>
    static constexpr bool isInvocable = std::is_invocable_v<F, Object &>;

    using WriteLock = WriteLockTemplate<Mutex>;
    using ReadLock = ReadLockTemplate<Mutex>;

    static_assert(!std::is_reference_v<Object>);
    static_assert(!notifiable);

    /**
     * @brief Construct a new guarded @p Object
     * 
     * @tparam Args 
     * @param args Arguments passed to the constructor of @p Object
     */
    template<typename... Args>
    Guard(Args &&... args)
        : _object(std::forward<Args>(args)...)
    {}

    /**
     * @brief Construct a new guarded @p Object 
     * 
     * @tparam T 
     * @param list `std::initializer` of @p T passed to the constructor of @p Object
     */
    template<typename T>
    Guard(std::initializer_list<T> list)
        : _object(list)
    {}

    /**
     * @brief Access to proxy object with locked mutex. Lock use write semantic.
     * 
     * @tparam F Callable object accepting `Object &` as a parameter
     * @param f functor
     * @return Whatever @p f returns
     */
    template<typename F, typename = std::enable_if_t<isInvocable<F>>>
    auto lock(F f) {
        WriteLock _(_mutex);
        return f(_object);
    }

    /**
     * @brief Access to proxy object with locked mutex. Lock use read semantic.
     * 
     * @tparam F Callable object accepting `const Object &` as a parameter
     * @param f functor
     * @return Whatever @p f returns
     */
    template<typename F, typename = std::enable_if_t<isInvocable<F>>>
    auto lock(F f) const {
        ReadLock _(_mutex);
        return f(_object);
    }

    /**
     * @brief Access to proxy object with locked mutex. Lock use read semantic.
     * 
     * @tparam F Callable object accepting `const Object &` as a parameter
     * @param f functor
     * @return Whatever @p f returns
     */
    template<typename F, typename = std::enable_if_t<isInvocable<F>>>
    auto constLock(F f) const {
        ReadLock _(_mutex);
        return f(_object);
    }

    /**
     * @brief Creates a proxy object with write (exclusive) access to the object.
     * 
     * @see Proxy
     * 
     * @return Proxy<Object &, WriteLock>
     */
    Proxy<Object, WriteLock> lock() {
        return {_object, WriteLock(_mutex)};
    }

    /**
     * @brief Creates a proxy object with read (shared) access to the object.
     * 
     * @see Proxy
     * 
     * @return Proxy<const Object &, WriteLock>
     */
    Proxy<const Object, ReadLock> lock() const {
        return {_object, ReadLock(_mutex)};
    }

    /**
     * @brief Creates a proxy object with read (shared) access to the object.
     * 
     * @see Proxy
     * 
     * @return Proxy<const Object &, WriteLock>
     */
    Proxy<const Object, ReadLock> constLock() const {
        return {_object, ReadLock(_mutex)};
    }

    void swap(Guard &other);

private:
    template<typename F, typename... Guards>
    friend auto lockAll(F f, Guards &&... guards);

    mutable Mutex _mutex;
    Object _object;
};

template<
    typename Object,
    typename Mutex,
    template<typename> class LockTemplate
>
struct alignas(hardware_destructive_interference_size) Guard<Object, Mutex, LockTemplate, LockTemplate, true> {

    template<typename F>
    static constexpr bool isInvocable = std::is_invocable_v<F, Object &>;

    struct SignalableLock : detail::SignalableLock {

        SignalableLock(Mutex &mutex, EnableConditionNotification &object)
            : _lock(mutex)
            , _object(&object)
        {
            _object->setSignalableLock(this);
        }

        SignalableLock(Mutex &mutex)
            : _lock(mutex)
            , _object(nullptr)
        {}

        SignalableLock(SignalableLock &&other)
            : _lock(std::move(other._lock))
            , _object(other._object)
        {
            if (_object)
                _object->setSignalableLock(this);
            other._object = nullptr;
        }

        ~SignalableLock() {
            if (_object)
                _object->setSignalableLock(nullptr);
        }

        void lock() override {
            _lock.lock();
        }

        bool try_lock() override {
            return _lock.try_lock();
        }

        void unlock() override {
            _lock.unlock();
        }

    private:
        LockTemplate<Mutex> _lock;
        EnableConditionNotification *_object;
    };

    static_assert(!std::is_reference_v<Object>);

    /**
     * @brief Construct a new guarded @p Object
     * 
     * @tparam Args 
     * @param args Arguments passed to the constructor of @p Object
     */
    template<typename... Args>
    Guard(Args &&... args)
        : _object(std::forward<Args>(args)...)
    {}

    /**
     * @brief Construct a new guarded @p Object 
     * 
     * @tparam T 
     * @param list `std::initializer` of @p T passed to the constructor of @p Object
     */
    template<typename T>
    Guard(std::initializer_list<T> list)
        : _object(list)
    {}

    /**
     * @brief Access to proxy object with locked mutex. Lock use write semantic.
     * 
     * @tparam F Callable object accepting `Object &` as a parameter
     * @param f functor
     * @return Whatever @p f returns
     */
    template<typename F, typename = std::enable_if_t<isInvocable<F>>>
    auto lock(F f) {
        SignalableLock _(_mutex, Traits<Object>::access(_object));
        return f(_object);
    }

    /**
     * @brief Access to proxy object with locked mutex. Lock use read semantic.
     * 
     * @tparam F Callable object accepting `const Object &` as a parameter
     * @param f functor
     * @return Whatever @p f returns
     */
    template<typename F, typename = std::enable_if_t<isInvocable<F>>>
    auto lock(F f) const {
        SignalableLock _(_mutex);
        return f(_object);
    }

    /**
     * @brief Access to proxy object with locked mutex. Lock use read semantic.
     * 
     * @tparam F Callable object accepting `const Object &` as a parameter
     * @param f functor
     * @return Whatever @p f returns
     */
    template<typename F, typename = std::enable_if_t<isInvocable<F>>>
    auto constLock(F f) const {
        SignalableLock _(_mutex);
        return f(_object);
    }

    /**
     * @brief Creates a proxy object with write (exclusive) access to the object.
     * 
     * @see Proxy
     * 
     * @return Proxy<Object &, WriteLock>
     */
    Proxy<Object, SignalableLock> lock() {
        return {_object, SignalableLock(_mutex,  Traits<Object>::access(_object))};
    }

    /**
     * @brief Creates a proxy object with read (shared) access to the object.
     * 
     * @see Proxy
     * 
     * @return Proxy<const Object &, WriteLock>
     */
    Proxy<const Object, SignalableLock> lock() const {
        return {_object, _mutex};
    }

    /**
     * @brief Creates a proxy object with read (shared) access to the object.
     * 
     * @see Proxy
     * 
     * @return Proxy<const Object &, WriteLock>
     */
    Proxy<const Object, SignalableLock> constLock() const {
        return {_object, _mutex};
    }

    /**
     * @brief Notify one thread waiting on conditional variable. No lock are 
     * 
     */
    void notifyOne() {
        getSignalable().notifyOne();
    }

    void notifyAll() {
        getSignalable().notifyAll();
    }

private:
    template<typename F, typename... Guards>
    friend auto lockAll(F f, Guards &&... guards);

    EnableConditionNotification &getSignalable() {
        return Traits<Object>::access(_object);
    }

    Object _object;
    mutable Mutex _mutex;

};

/**
 * @brief Access to all guarded objects using deadlock avoidance algorithm.
 * 
 * @tparam F Callable object accepting all objects in parameters
 * @tparam Guards
 * @param f functor
 * @param guards All guards you wish to lock.
 * @return Whatever `f` returns
 */
template<typename F, typename... Guards>
inline auto lockAll(F f, Guards &&... guards) {
    std::scoped_lock lock(guards._mutex...);
    return f(guards._object...);
}

template<
    typename Object,
    typename Mutex,
    template<typename> class WriteLockTemplate,
    template<typename> class ReadLockTemplate,
    bool notSignalable
>
void Guard<Object, Mutex, WriteLockTemplate, ReadLockTemplate, notSignalable>::swap(Guard<Object, Mutex, WriteLockTemplate, ReadLockTemplate, notSignalable> &other) {
    lockAll([&](Object &lhs, Object &rhs) {
        using std::swap;
        swap(lhs, rhs);
    }, *this, other);
}

/**
 * @brief Implements `std::lock_guard` with atmost @p Duration @p Units time of waiting before
 *      throwing an exception to avoid deadlock. Meets BasicSignalable requirements.
 * 
 * @warning User is responsible of chosing the right amount of time to wait.
 * 
 * @tparam Mutex
 * @tparam Duration number of @p Units
 * @tparam Unit `std::chrono::duration` template
 */
template<
    typename Mutex,
    unsigned Duration,
    typename Unit
>
struct TimedLock {

    explicit TimedLock(Mutex &mutex)
        : _mutex(&mutex)
    {
        lock();
    }

    ~TimedLock() {
        if (_mutex)
            _mutex->unlock();
    }

    TimedLock(TimedLock &&other)
        : _mutex(other._mutex)
    {
        other._mutex = nullptr;
    }

    TimedLock(const TimedLock &) = delete;

    TimedLock &operator=(TimedLock other) {
        using std::swap;
        swap(_mutex, other._mutex);
        return *this;
    }

    void lock() {
        const auto timeout = std::chrono::steady_clock::now() + Unit(Duration);
        while (!_mutex->try_lock_until(timeout)) {
            if (timeout < std::chrono::steady_clock::now())
                throw std::system_error(std::make_error_code(std::errc::resource_deadlock_would_occur));
        }
    }

    bool try_lock() {
        return _mutex->try_lock();
    }

    void unlock() {
        _mutex->unlock();
    }

private:
    Mutex *_mutex;
};

template<typename Mutex>
using TimedLockFor3seconds = TimedLock<Mutex, 3, std::chrono::seconds>;

} // namespace detail

using detail::lockAll;
using detail::EnableConditionNotification;

/**
 * @brief Guard with exclusive access to the @p Object .
 * 
 * @tparam Object object
 */
template<typename Object>
using Exclusive = detail::Guard<Object, std::mutex, std::unique_lock>;

/**
 * @brief Guard with exclusive and shared access to the @p Object .
 * 
 * @tparam Object object
 */
template<typename Object>
using Shared = detail::Guard<Object, std::shared_mutex, std::unique_lock, std::shared_lock>;

/**
 * @brief Guard with exclusive recursive access to the @p Object . Timeout for waiting for mutex is 3 seconds.
 *        This class **should** not be used unless the architecture has poor design.
 * 
 * @tparam Object object
 */
template<typename Object>
using Recursive = detail::Guard<Object, std::recursive_timed_mutex, detail::TimedLockFor3seconds>;


} // namespace parallel::guard

#endif
