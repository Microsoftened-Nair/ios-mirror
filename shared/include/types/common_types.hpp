#pragma once

#include <cassert>
#include <concepts>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace shared {

// Convenience type aliases for zero-overhead non-owning buffer views
using ReadOnlyBuffer = std::span<const uint8_t>;
using MutableBuffer = std::span<uint8_t>;

// Error wrapper tag type for explicit error construction
template <typename E>
struct Err {
    E value;

    template <typename... Args>
        requires std::is_constructible_v<E, Args...>
    constexpr explicit Err(Args&&... args) : value(std::forward<Args>(args)...) {}
};

template <typename E>
Err(E) -> Err<E>;

template <typename T>
struct is_err_wrapper : std::false_type {};

template <typename E>
struct is_err_wrapper<Err<E>> : std::true_type {};

template <typename T>
inline constexpr bool is_err_wrapper_v = is_err_wrapper<T>::value;

namespace detail {
struct uninitialized_tag {};
} // namespace detail

/**
 * @brief Zero-overhead, allocation-free Result monad.
 *
 * Stores either a value of type T or an error of type E using union storage
 * discriminated by a single boolean flag. Fully Rule-of-Five and C++20 compliant.
 */
template <typename T, typename E>
class Result {
private:
    union Storage {
        T value_;
        E error_;

        constexpr Storage() {}
        constexpr ~Storage() {}
    } storage_;

    bool is_ok_{false};

    constexpr explicit Result(detail::uninitialized_tag) noexcept {}

public:
    using value_type = T;
    using error_type = E;

    // Default constructor when T is default-constructible
    constexpr Result()
        requires std::is_default_constructible_v<T>
        : is_ok_(true) {
        std::construct_at(&storage_.value_);
    }

    // Value constructor
    template <typename U = T>
        requires (!std::is_same_v<std::decay_t<U>, Result> &&
                  !is_err_wrapper_v<std::decay_t<U>> &&
                  std::is_constructible_v<T, U>)
    constexpr Result(U&& val) : is_ok_(true) {
        std::construct_at(&storage_.value_, std::forward<U>(val));
    }

    // Error constructor via Err wrapper
    template <typename U>
        requires std::is_constructible_v<E, U>
    constexpr Result(const Err<U>& err) : is_ok_(false) {
        std::construct_at(&storage_.error_, err.value);
    }

    template <typename U>
        requires std::is_constructible_v<E, U>
    constexpr Result(Err<U>&& err) : is_ok_(false) {
        std::construct_at(&storage_.error_, std::move(err.value));
    }

    // Direct error constructor when U can construct E but cannot construct T
    template <typename U = E>
        requires (!std::is_same_v<std::decay_t<U>, Result> &&
                  !is_err_wrapper_v<std::decay_t<U>> &&
                  !std::is_same_v<T, E> &&
                  !std::is_constructible_v<T, U> &&
                  std::is_constructible_v<E, U>)
    constexpr explicit(!std::is_convertible_v<U, E>) Result(U&& err) : is_ok_(false) {
        std::construct_at(&storage_.error_, std::forward<U>(err));
    }

    // Rule of Five: Destructor
    constexpr ~Result() {
        destroy();
    }

    // Rule of Five: Copy constructor
    constexpr Result(const Result& other)
        requires (std::is_copy_constructible_v<T> && std::is_copy_constructible_v<E>)
        : is_ok_(other.is_ok_) {
        if (is_ok_) {
            std::construct_at(&storage_.value_, other.storage_.value_);
        } else {
            std::construct_at(&storage_.error_, other.storage_.error_);
        }
    }

    // Rule of Five: Move constructor
    constexpr Result(Result&& other) noexcept(
        std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_constructible_v<E>)
        requires (std::is_move_constructible_v<T> && std::is_move_constructible_v<E>)
        : is_ok_(other.is_ok_) {
        if (is_ok_) {
            std::construct_at(&storage_.value_, std::move(other.storage_.value_));
        } else {
            std::construct_at(&storage_.error_, std::move(other.storage_.error_));
        }
    }

    // Rule of Five: Copy assignment
    constexpr Result& operator=(const Result& other)
        requires (std::is_copy_constructible_v<T> && std::is_copy_constructible_v<E> &&
                  std::is_copy_assignable_v<T> && std::is_copy_assignable_v<E>) {
        if (this == &other) {
            return *this;
        }
        if (is_ok_ && other.is_ok_) {
            storage_.value_ = other.storage_.value_;
        } else if (!is_ok_ && !other.is_ok_) {
            storage_.error_ = other.storage_.error_;
        } else {
            destroy();
            is_ok_ = other.is_ok_;
            if (is_ok_) {
                std::construct_at(&storage_.value_, other.storage_.value_);
            } else {
                std::construct_at(&storage_.error_, other.storage_.error_);
            }
        }
        return *this;
    }

    // Rule of Five: Move assignment
    constexpr Result& operator=(Result&& other) noexcept(
        std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_constructible_v<E> &&
        std::is_nothrow_move_assignable_v<T> && std::is_nothrow_move_assignable_v<E>)
        requires (std::is_move_constructible_v<T> && std::is_move_constructible_v<E> &&
                  std::is_move_assignable_v<T> && std::is_move_assignable_v<E>) {
        if (this == &other) {
            return *this;
        }
        if (is_ok_ && other.is_ok_) {
            storage_.value_ = std::move(other.storage_.value_);
        } else if (!is_ok_ && !other.is_ok_) {
            storage_.error_ = std::move(other.storage_.error_);
        } else {
            destroy();
            is_ok_ = other.is_ok_;
            if (is_ok_) {
                std::construct_at(&storage_.value_, std::move(other.storage_.value_));
            } else {
                std::construct_at(&storage_.error_, std::move(other.storage_.error_));
            }
        }
        return *this;
    }

    // Static in-place factories
    template <typename... Args>
        requires std::is_constructible_v<T, Args...>
    static constexpr Result ok(Args&&... args) {
        Result res(detail::uninitialized_tag{});
        res.is_ok_ = true;
        std::construct_at(&res.storage_.value_, std::forward<Args>(args)...);
        return res;
    }

    template <typename... Args>
        requires std::is_constructible_v<E, Args...>
    static constexpr Result err(Args&&... args) {
        Result res(detail::uninitialized_tag{});
        res.is_ok_ = false;
        std::construct_at(&res.storage_.error_, std::forward<Args>(args)...);
        return res;
    }

    // Status queries
    constexpr bool is_ok() const noexcept { return is_ok_; }
    constexpr bool is_err() const noexcept { return !is_ok_; }
    constexpr explicit operator bool() const noexcept { return is_ok_; }

    // Value accessors
    constexpr const T& value() const & noexcept {
        assert(is_ok_);
        return storage_.value_;
    }

    constexpr T& value() & noexcept {
        assert(is_ok_);
        return storage_.value_;
    }

    constexpr const T&& value() const && noexcept {
        assert(is_ok_);
        return std::move(storage_.value_);
    }

    constexpr T&& value() && noexcept {
        assert(is_ok_);
        return std::move(storage_.value_);
    }

    // Error accessors
    constexpr const E& error() const & noexcept {
        assert(!is_ok_);
        return storage_.error_;
    }

    constexpr E& error() & noexcept {
        assert(!is_ok_);
        return storage_.error_;
    }

    constexpr const E&& error() const && noexcept {
        assert(!is_ok_);
        return std::move(storage_.error_);
    }

    constexpr E&& error() && noexcept {
        assert(!is_ok_);
        return std::move(storage_.error_);
    }

    // Value or fallback
    template <typename U>
    constexpr T value_or(U&& default_value) const & {
        return is_ok_ ? storage_.value_ : static_cast<T>(std::forward<U>(default_value));
    }

    template <typename U>
    constexpr T value_or(U&& default_value) && {
        return is_ok_ ? std::move(storage_.value_) : static_cast<T>(std::forward<U>(default_value));
    }

    // Monadic transform / map
    template <typename F>
    constexpr auto transform(F&& f) const & {
        using Ret = std::invoke_result_t<F, const T&>;
        if constexpr (std::is_void_v<Ret>) {
            if (is_ok()) {
                std::forward<F>(f)(storage_.value_);
                return Result<void, E>::ok();
            } else {
                return Result<void, E>::err(storage_.error_);
            }
        } else {
            if (is_ok()) {
                return Result<Ret, E>::ok(std::forward<F>(f)(storage_.value_));
            } else {
                return Result<Ret, E>::err(storage_.error_);
            }
        }
    }

    template <typename F>
    constexpr auto map(F&& f) const & {
        return transform(std::forward<F>(f));
    }

    // Monadic and_then
    template <typename F>
    constexpr auto and_then(F&& f) const & {
        using Ret = std::invoke_result_t<F, const T&>;
        if (is_ok()) {
            return std::forward<F>(f)(storage_.value_);
        } else {
            return Ret::err(storage_.error_);
        }
    }

    // Monadic error transform / map_error
    template <typename F>
    constexpr auto transform_error(F&& f) const & {
        using NewE = std::invoke_result_t<F, const E&>;
        if (is_ok()) {
            return Result<T, NewE>::ok(storage_.value_);
        } else {
            return Result<T, NewE>::err(std::forward<F>(f)(storage_.error_));
        }
    }

    template <typename F>
    constexpr auto map_error(F&& f) const & {
        return transform_error(std::forward<F>(f));
    }

    // Monadic or_else
    template <typename F>
    constexpr auto or_else(F&& f) const & {
        using Ret = std::invoke_result_t<F, const E&>;
        if (is_ok()) {
            return Ret::ok(storage_.value_);
        } else {
            return std::forward<F>(f)(storage_.error_);
        }
    }

private:
    constexpr void destroy() noexcept {
        if (is_ok_) {
            std::destroy_at(&storage_.value_);
        } else {
            std::destroy_at(&storage_.error_);
        }
    }
};

/**
 * @brief Explicit specialization of Result for void value type.
 *
 * Uses dummy union storage to satisfy layout constraints without dynamic heap allocation.
 */
template <typename E>
class Result<void, E> {
private:
    union Storage {
        char dummy_;
        E error_;

        constexpr Storage() : dummy_(0) {}
        constexpr ~Storage() {}
    } storage_;

    bool is_ok_{true};

    constexpr explicit Result(detail::uninitialized_tag) noexcept {}

public:
    using value_type = void;
    using error_type = E;

    // Default constructor creates an OK void result
    constexpr Result() noexcept : is_ok_(true) {
        storage_.dummy_ = 0;
    }

    // Error constructor via Err wrapper
    template <typename U>
        requires std::is_constructible_v<E, U>
    constexpr Result(const Err<U>& err) : is_ok_(false) {
        std::construct_at(&storage_.error_, err.value);
    }

    template <typename U>
        requires std::is_constructible_v<E, U>
    constexpr Result(Err<U>&& err) : is_ok_(false) {
        std::construct_at(&storage_.error_, std::move(err.value));
    }

    // Direct error constructor for void specialization
    template <typename U = E>
        requires (!std::is_same_v<std::decay_t<U>, Result> &&
                  !is_err_wrapper_v<std::decay_t<U>> &&
                  std::is_constructible_v<E, U>)
    constexpr explicit(!std::is_convertible_v<U, E>) Result(U&& err) : is_ok_(false) {
        std::construct_at(&storage_.error_, std::forward<U>(err));
    }

    // Rule of Five: Destructor
    constexpr ~Result() {
        destroy();
    }

    // Rule of Five: Copy constructor
    constexpr Result(const Result& other)
        requires std::is_copy_constructible_v<E>
        : is_ok_(other.is_ok_) {
        if (!is_ok_) {
            std::construct_at(&storage_.error_, other.storage_.error_);
        } else {
            storage_.dummy_ = 0;
        }
    }

    // Rule of Five: Move constructor
    constexpr Result(Result&& other) noexcept(std::is_nothrow_move_constructible_v<E>)
        requires std::is_move_constructible_v<E>
        : is_ok_(other.is_ok_) {
        if (!is_ok_) {
            std::construct_at(&storage_.error_, std::move(other.storage_.error_));
        } else {
            storage_.dummy_ = 0;
        }
    }

    // Rule of Five: Copy assignment
    constexpr Result& operator=(const Result& other)
        requires (std::is_copy_constructible_v<E> && std::is_copy_assignable_v<E>) {
        if (this == &other) {
            return *this;
        }
        if (!is_ok_ && !other.is_ok_) {
            storage_.error_ = other.storage_.error_;
        } else if (is_ok_ && other.is_ok_) {
            // Both ok, nothing to copy
        } else if (!is_ok_ && other.is_ok_) {
            destroy();
            is_ok_ = true;
            storage_.dummy_ = 0;
        } else {
            std::construct_at(&storage_.error_, other.storage_.error_);
            is_ok_ = false;
        }
        return *this;
    }

    // Rule of Five: Move assignment
    constexpr Result& operator=(Result&& other) noexcept(
        std::is_nothrow_move_constructible_v<E> && std::is_nothrow_move_assignable_v<E>)
        requires (std::is_move_constructible_v<E> && std::is_move_assignable_v<E>) {
        if (this == &other) {
            return *this;
        }
        if (!is_ok_ && !other.is_ok_) {
            storage_.error_ = std::move(other.storage_.error_);
        } else if (is_ok_ && other.is_ok_) {
            // Both ok, nothing to move
        } else if (!is_ok_ && other.is_ok_) {
            destroy();
            is_ok_ = true;
            storage_.dummy_ = 0;
        } else {
            std::construct_at(&storage_.error_, std::move(other.storage_.error_));
            is_ok_ = false;
        }
        return *this;
    }

    // Static factories
    static constexpr Result ok() noexcept {
        return Result();
    }

    template <typename... Args>
        requires std::is_constructible_v<E, Args...>
    static constexpr Result err(Args&&... args) {
        Result res(detail::uninitialized_tag{});
        res.is_ok_ = false;
        std::construct_at(&res.storage_.error_, std::forward<Args>(args)...);
        return res;
    }

    // Status queries
    constexpr bool is_ok() const noexcept { return is_ok_; }
    constexpr bool is_err() const noexcept { return !is_ok_; }
    constexpr explicit operator bool() const noexcept { return is_ok_; }

    // Value accessor
    constexpr void value() const noexcept {
        assert(is_ok_);
    }

    // Error accessors
    constexpr const E& error() const & noexcept {
        assert(!is_ok_);
        return storage_.error_;
    }

    constexpr E& error() & noexcept {
        assert(!is_ok_);
        return storage_.error_;
    }

    constexpr const E&& error() const && noexcept {
        assert(!is_ok_);
        return std::move(storage_.error_);
    }

    constexpr E&& error() && noexcept {
        assert(!is_ok_);
        return std::move(storage_.error_);
    }

    // Monadic transform / map
    template <typename F>
    constexpr auto transform(F&& f) const & {
        using Ret = std::invoke_result_t<F>;
        if constexpr (std::is_void_v<Ret>) {
            if (is_ok()) {
                std::forward<F>(f)();
                return Result<void, E>::ok();
            } else {
                return Result<void, E>::err(storage_.error_);
            }
        } else {
            if (is_ok()) {
                return Result<Ret, E>::ok(std::forward<F>(f)());
            } else {
                return Result<Ret, E>::err(storage_.error_);
            }
        }
    }

    template <typename F>
    constexpr auto map(F&& f) const & {
        return transform(std::forward<F>(f));
    }

    // Monadic and_then
    template <typename F>
    constexpr auto and_then(F&& f) const & {
        using Ret = std::invoke_result_t<F>;
        if (is_ok()) {
            return std::forward<F>(f)();
        } else {
            return Ret::err(storage_.error_);
        }
    }

    // Monadic error transform / map_error
    template <typename F>
    constexpr auto transform_error(F&& f) const & {
        using NewE = std::invoke_result_t<F, const E&>;
        if (is_ok()) {
            return Result<void, NewE>::ok();
        } else {
            return Result<void, NewE>::err(std::forward<F>(f)(storage_.error_));
        }
    }

    template <typename F>
    constexpr auto map_error(F&& f) const & {
        return transform_error(std::forward<F>(f));
    }

    // Monadic or_else
    template <typename F>
    constexpr auto or_else(F&& f) const & {
        using Ret = std::invoke_result_t<F, const E&>;
        if (is_ok()) {
            return Ret::ok();
        } else {
            return std::forward<F>(f)(storage_.error_);
        }
    }

private:
    constexpr void destroy() noexcept {
        if (!is_ok_) {
            std::destroy_at(&storage_.error_);
        }
    }
};

} // namespace shared
