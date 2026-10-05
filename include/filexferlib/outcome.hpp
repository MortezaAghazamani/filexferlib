#pragma once
#include <system_error>
#include <optional>
#include <utility>
#include <string>
#include <stdexcept>

namespace filexferlib {

/**
 * @brief Result type for exception-free APIs.
 *
 * `Outcome<T>` holds either a value of type `T` or an `std::error_code`
 * (with an optional message). Use it like:
 *
 * @code
 * auto r = client.tryConnect(host, port).get();
 * if (!r) {
 *     std::cerr << "error: " << r.error().message()
 *               << " (" << r.message() << ")\n";
 *     return 1;
 * }
 * auto& info = r.value();
 * @endcode
 *
 * `Outcome<void>` is a specialization that holds only success/failure
 * without a value.
 */
template <typename T>
class Outcome {
public:
    // ---- Success constructors ----
    Outcome() = default;

    Outcome(T value)
        : value_(std::move(value)) {}

    // ---- Failure constructors ----
    Outcome(std::error_code ec)
        : error_(ec) {}

    Outcome(std::error_code ec, std::string msg)
        : error_(ec), message_(std::move(msg)) {}

    // ---- Status ----
    bool ok() const noexcept { return !error_; }
    explicit operator bool() const noexcept { return ok(); }

    // ---- Access value (only valid if ok()) ----
    T& value() & {
        if (!value_) throw std::logic_error("Outcome::value() on error");
        return *value_;
    }

    const T& value() const& {
        if (!value_) throw std::logic_error("Outcome::value() on error");
        return *value_;
    }

    T&& value() && {
        if (!value_) throw std::logic_error("Outcome::value() on error");
        return std::move(*value_);
    }

    T value_or(T fallback) const {
        return value_ ? *value_ : std::move(fallback);
    }

    T* operator->() {
        if (!value_) throw std::logic_error("Outcome::operator-> on error");
        return &*value_;
    }

    const T* operator->() const {
        if (!value_) throw std::logic_error("Outcome::operator-> on error");
        return &*value_;
    }

    T& operator*() { return value(); }
    const T& operator*() const { return value(); }

    // ---- Error info ----
    std::error_code error() const noexcept { return error_; }
    const std::string& message() const noexcept { return message_; }

private:
    std::optional<T> value_;
    std::error_code  error_;
    std::string      message_;
};

// ---- void specialization ----
template <>
class Outcome<void> {
public:
    Outcome() = default;

    Outcome(std::error_code ec)
        : error_(ec) {}

    Outcome(std::error_code ec, std::string msg)
        : error_(ec), message_(std::move(msg)) {}

    bool ok() const noexcept { return !error_; }
    explicit operator bool() const noexcept { return ok(); }

    std::error_code error() const noexcept { return error_; }
    const std::string& message() const noexcept { return message_; }

private:
    std::error_code error_;
    std::string     message_;
};

} // namespace filexferlib
