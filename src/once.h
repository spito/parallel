#ifndef PARALLEL_ONCE_H
#define PARALLEL_ONCE_H

#include <atomic>
#include <initializer_list>
#include <optional>
#include <type_traits>

#include "platform.h"

/**
 * @author  "Jiri Weiser" <jiri.weiser@gmail.com>
 * @date    2021-01-13
 * 
 * Guards for initialization:
 *      parallel::Once
 *      parallel::OnceConst
 * 
 *      Both these classes allows you to initialize a shared object at most once. Furthemore, using method `disable`,
 *      one may disallow future initializations. It is a responsibility of the wrapped object to be thread-safe.
 * 
 */

namespace parallel {
namespace detail {

/**
 * @brief Base class wrapper for once initialized object.
 * 
 * @tparam Object Object to be wrapped
 * @tparam ForceOptional If set to @p true , the object itself is wrapped by @p std::optional .
 *      Otherwise the usage of @p std::optional is turned on when the object is not default constructible
 *      nor move assignable.
 */
template<typename Object, bool ForceOptional>
class alignas(hardware_destructive_interference_size) Once {

    static constexpr bool UseOptional = ForceOptional
        || !(std::is_default_constructible_v<Object> && std::is_move_assignable_v<Object>);

    using Type = std::conditional_t<UseOptional, std::optional<Object>, Object>;

public:
    /**
     * @brief Construct a new Once object. Default constructor is called when @p UseOptional is @p false ,
     *      no object is initialized otherwise.
     * 
     */
    Once()
        : _flags(EMPTY)
    {}

    /**
     * @brief Create the object by move assignment from @p object .
     * 
     * @param object Object to be moved into the wrapped object.
     * @throw whatever move assignment of @p Object throws
     * @return true Object was created.
     * @return false Object was already created.
     */
    bool operator=(Object object) {
        return init([&] { _object = std::move(object); });
    }

    /**
     * @brief Create the object by move assignment of lately created value.
     * 
     * @tparam Initializator Late creator. Whatever it returns, @p Object must be able to move assign the value.
     * @param initializator Functor; called only when object was not already created.
     * @throw whatever move assignment of @p Object or @p initializator throws
     * @return true Object was created.
     * @return false Object was already created.
     */
    template<typename Initializator>
    auto operator=(Initializator initializator)
        -> std::enable_if_t<std::is_invocable_v<Initializator>, bool>
    {
        return init([&]{ _object = std::move(initializator()); });
    }

    /**
     * @brief Create the object using @p initializator
     * 
     * @tparam Initializator Late creator. Accepts `Object &` or `std::optional<Object> &` depending on @p UseOptional .
     * @param initializator Functor; called only when object was not already created.
     * @throw whatever @p initializator throws
     * @return true Object was created.
     * @return false Object was already created.
     */
    template<typename Initializator>
    auto create(Initializator initializator)
        -> std::enable_if_t<std::is_invocable_v<Initializator, Type &>, bool>
    {
        return init([&]{ initializator(_object); });
    }

    /**
     * @brief Emplace the object. @p emplace method of `std::optional` is called if @p UseOptional ,
     *      move assignment of @p Object is performed otherwise
     * 
     * @tparam Args Argument list
     * @param args Arguments passed into the constructor of @p Object .
     * @throw whatever constructor or move assignment of @p Object throws
     * @return true Object was created.
     * @return false Object was already created.
     */
    template<typename... Args>
    auto emplace(Args &&... args)
        -> std::enable_if_t<std::is_constructible_v<Object, Args...>, bool>
    {
        return init([&] {
            if constexpr (UseOptional)
                _object.emplace(std::forward<Args>(args)...);
            else
                _object = Object(std::forward<Args>(args)...);
        });
    }

protected:
    template<typename Self>
    using ReturnType = std::conditional_t<std::is_const_v<Self>, const Object *, Object *>;

    template<typename Self>
    static ReturnType<Self> get(Self *self) noexcept {

        while (true) {
            int flags = self->_flags;

            if (flags & INITIALIZATION) // unlikely
                continue;

            if (flags & HAS_VALUE) {
                if constexpr (UseOptional)
                    return &self->_object.value();
                else
                    return &self->_object;
            }
            return nullptr;
        }
    }

    template<typename Self>
    static ReturnType<Self> disable(Self *self) noexcept {
        int expected = self->_flags;
        int desired;
        do {
            if (expected & DISABLED)
                break;

            if (expected & INITIALIZATION) // unlikely
                expected = HAS_VALUE;

            desired = expected | DISABLED;
        } while (!self->_flags.compare_exchange_strong(expected, desired));
        return get(self);
    }

private:
    template<typename How>
    bool init(How how) {
        int expected = EMPTY;
        if (!_flags.compare_exchange_strong(expected, INITIALIZATION))
            return false;

        try {
            how();
        } catch (...) {
            _flags = expected;
            throw;
        }

        _flags = HAS_VALUE;
        return true;
    }

    static constexpr int EMPTY          = 0b000;
    static constexpr int INITIALIZATION = 0b001;
    static constexpr int HAS_VALUE      = 0b010;
    static constexpr int DISABLED       = 0b100;

    mutable std::atomic<int> _flags;
    Type _object;
};

} // namespace detail

/**
 * @brief Class wrapper for once initialized object.
 * 
 * @see detail::Once
 * 
 * @tparam Object object
 * @tparam ForceOptional defaults to @p false
 */
template<
    typename Object,
    bool ForceOptional = false
>
struct Once : detail::Once<Object, ForceOptional> {
    using Base = detail::Once<Object, ForceOptional>;
    using Base::operator=;

    /**
     * @brief Access to the @p Object .
     * 
     * @return Object* @p nullptr in case object was not already created, valid pointer instead.
     */
    Object *get() noexcept {
        return Base::get(this);
    }

    /**
     * @brief Constant access to the @p Object .
     * 
     * @return Object* @p nullptr in case object was not already created, valid pointer instead.
     */
    const Object *get() const noexcept {
        return Base::get(this);
    }

    /**
     * @brief Disable creation in the future. Access to the @p Object .
     * 
     * @return Object* @p nullptr in case object was not already created, valid pointer instead.
     */
    Object *disable() noexcept {
        return Base::disable(this);
    }

    /**
     * @brief Disable creation in the future. Constant access to the @p Object .
     * 
     * @return Object* @p nullptr in case object was not already created, valid pointer instead.
     */
    const Object *disable() const noexcept {
        return Base::disable(this);
    }
};

/**
 * @brief Class wrapper for once initialized object. Provides only constant access methods.
 * 
 * @see detail::Once
 * 
 * @tparam Object object
 * @tparam ForceOptional defaults to @p false
 */
template<
    typename Object,
    bool ForceOptional = false
>
struct OnceConst : detail::Once<Object, ForceOptional> {
    using Base = detail::Once<Object, ForceOptional>;
    using Base::operator=;

    /**
     * @brief Constant access to the @p Object .
     * 
     * @return Object* @p nullptr in case object was not already created, valid pointer instead.
     */
    const Object *get() const noexcept {
        return Base::get(this);
    }

    /**
     * @brief Disable creation in the future. Constant access to the @p Object .
     * 
     * @return Object* @p nullptr in case object was not already created, valid pointer instead.
     */
    const Object *disable() const noexcept {
        return Base::disable(this);
    }
};

} // namespace parallel

#endif