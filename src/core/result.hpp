#pragma once

#include <optional>
#include <string>
#include <variant>

namespace osc {

struct Error {
    std::string message;

    Error() = default;
    explicit Error(std::string msg) : message(std::move(msg)) {}
};

/// Simple Result type: holds either a value of type T or an Error.
/// For void results, use Result<void>.
template <typename T>
class Result {
public:
    Result(T value) : data_(std::move(value)) {}
    Result(Error err) : data_(std::move(err)) {}

    bool ok() const { return std::holds_alternative<T>(data_); }
    explicit operator bool() const { return ok(); }

    const T& value() const { return std::get<T>(data_); }
    T& value() { return std::get<T>(data_); }

    const Error& error() const { return std::get<Error>(data_); }

private:
    std::variant<T, Error> data_;
};

/// Specialization for void results.
template <>
class Result<void> {
public:
    Result() : err_(std::nullopt) {}
    Result(Error err) : err_(std::move(err)) {}

    bool ok() const { return !err_.has_value(); }
    explicit operator bool() const { return ok(); }

    const Error& error() const { return err_.value(); }

private:
    std::optional<Error> err_;
};

} // namespace osc
