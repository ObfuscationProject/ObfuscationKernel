#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace ok {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using usize = std::size_t;
using uptr = std::uintptr_t;

enum class StatusCode : u32 {
    ok = 0,
    invalid_argument,
    no_memory,
    not_found,
    already_exists,
    busy,
    denied,
    unsupported,
    overflow,
    would_block,
    interrupted,
    fault,
    not_initialized,
};

class Status final {
public:
    constexpr Status() = default;
    constexpr Status(StatusCode code, std::string_view message = {}) : code_(code), message_(message) {}

    [[nodiscard]] static constexpr Status success() { return {}; }
    [[nodiscard]] static constexpr Status invalid_argument(std::string_view message = {}) { return {StatusCode::invalid_argument, message}; }
    [[nodiscard]] static constexpr Status no_memory(std::string_view message = {}) { return {StatusCode::no_memory, message}; }
    [[nodiscard]] static constexpr Status not_found(std::string_view message = {}) { return {StatusCode::not_found, message}; }
    [[nodiscard]] static constexpr Status already_exists(std::string_view message = {}) { return {StatusCode::already_exists, message}; }
    [[nodiscard]] static constexpr Status busy(std::string_view message = {}) { return {StatusCode::busy, message}; }
    [[nodiscard]] static constexpr Status denied(std::string_view message = {}) { return {StatusCode::denied, message}; }
    [[nodiscard]] static constexpr Status unsupported(std::string_view message = {}) { return {StatusCode::unsupported, message}; }
    [[nodiscard]] static constexpr Status overflow(std::string_view message = {}) { return {StatusCode::overflow, message}; }
    [[nodiscard]] static constexpr Status would_block(std::string_view message = {}) { return {StatusCode::would_block, message}; }
    [[nodiscard]] static constexpr Status interrupted(std::string_view message = {}) { return {StatusCode::interrupted, message}; }
    [[nodiscard]] static constexpr Status fault(std::string_view message = {}) { return {StatusCode::fault, message}; }
    [[nodiscard]] static constexpr Status not_initialized(std::string_view message = {}) { return {StatusCode::not_initialized, message}; }

    [[nodiscard]] constexpr bool ok() const { return code_ == StatusCode::ok; }
    [[nodiscard]] constexpr StatusCode code() const { return code_; }
    [[nodiscard]] constexpr std::string_view message() const { return message_; }
    [[nodiscard]] constexpr explicit operator bool() const { return ok(); }

private:
    StatusCode code_ {StatusCode::ok};
    std::string_view message_ {};
};

template <typename T>
class Result final {
public:
    Result(T value) : value_(std::move(value)), status_(Status::success()) {}
    Result(Status status) : value_(std::nullopt), status_(status) {}

    [[nodiscard]] static Result failure(Status status) { return Result(status); }

    [[nodiscard]] bool ok() const { return status_.ok(); }
    [[nodiscard]] Status status() const { return status_; }
    [[nodiscard]] explicit operator bool() const { return ok(); }

    [[nodiscard]] T& value() & { return *value_; }
    [[nodiscard]] const T& value() const& { return *value_; }
    [[nodiscard]] T&& value() && { return std::move(*value_); }

private:
    std::optional<T> value_;
    Status status_;
};

class NonCopyable {
protected:
    constexpr NonCopyable() = default;
    ~NonCopyable() = default;

public:
    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;
};

} // namespace ok

