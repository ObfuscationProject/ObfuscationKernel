#pragma once

#include <cstddef>
#include <concepts>
#include <optional>
#include <string_view>
#include <utility>

namespace ok
{

using u8 = __UINT8_TYPE__;
using u16 = __UINT16_TYPE__;
using u32 = __UINT32_TYPE__;
using u64 = __UINT64_TYPE__;
using i8 = __INT8_TYPE__;
using i16 = __INT16_TYPE__;
using i32 = __INT32_TYPE__;
using i64 = __INT64_TYPE__;
using usize = decltype(sizeof(0));
using uptr = __UINTPTR_TYPE__;

enum class StatusCode : u32
{
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
    broken_pipe,
    bad_descriptor,
    not_seekable,
    io_error,
    cross_device,
    not_directory,
    is_directory,
    not_empty,
    link_limit,
    no_space,
    permission,
    too_many_symlinks,
    process_file_limit,
    system_file_limit,
    name_too_long,
    message_size,
    no_process,
    not_terminal,
    exec_format,
    argument_list_too_long,
};

class Status final
{
  public:
    [[nodiscard]] static constexpr Status not_terminal(std::string_view message = {})
    {
        return {StatusCode::not_terminal, message};
    }
    constexpr Status() = default;
    constexpr Status(StatusCode code, std::string_view message = {}) : code_(code), message_(message)
    {
    }

    [[nodiscard]] static constexpr Status success()
    {
        return {};
    }
    [[nodiscard]] static constexpr Status broken_pipe(std::string_view message = {})
    {
        return {StatusCode::broken_pipe, message};
    }
    [[nodiscard]] static constexpr Status bad_descriptor(std::string_view message = {})
    {
        return {StatusCode::bad_descriptor, message};
    }
    [[nodiscard]] static constexpr Status not_seekable(std::string_view message = {})
    {
        return {StatusCode::not_seekable, message};
    }
    [[nodiscard]] static constexpr Status io_error(std::string_view message = {})
    {
        return {StatusCode::io_error, message};
    }
    [[nodiscard]] static constexpr Status cross_device(std::string_view message = {})
    {
        return {StatusCode::cross_device, message};
    }
    [[nodiscard]] static constexpr Status not_directory(std::string_view message = {})
    {
        return {StatusCode::not_directory, message};
    }
    [[nodiscard]] static constexpr Status is_directory(std::string_view message = {})
    {
        return {StatusCode::is_directory, message};
    }
    [[nodiscard]] static constexpr Status not_empty(std::string_view message = {})
    {
        return {StatusCode::not_empty, message};
    }
    [[nodiscard]] static constexpr Status too_many_symlinks(std::string_view message = {})
    {
        return {StatusCode::too_many_symlinks, message};
    }
    [[nodiscard]] static constexpr Status name_too_long(std::string_view message = {})
    {
        return {StatusCode::name_too_long, message};
    }
    [[nodiscard]] static constexpr Status message_size(std::string_view message = {})
    {
        return {StatusCode::message_size, message};
    }
    [[nodiscard]] static constexpr Status process_file_limit(std::string_view message = {})
    {
        return {StatusCode::process_file_limit, message};
    }
    [[nodiscard]] static constexpr Status system_file_limit(std::string_view message = {})
    {
        return {StatusCode::system_file_limit, message};
    }
    [[nodiscard]] static constexpr Status link_limit(std::string_view message = {})
    {
        return {StatusCode::link_limit, message};
    }
    [[nodiscard]] static constexpr Status no_space(std::string_view message = {})
    {
        return {StatusCode::no_space, message};
    }
    [[nodiscard]] static constexpr Status permission(std::string_view message = {})
    {
        return {StatusCode::permission, message};
    }
    [[nodiscard]] static constexpr Status invalid_argument(std::string_view message = {})
    {
        return {StatusCode::invalid_argument, message};
    }
    [[nodiscard]] static constexpr Status no_process(std::string_view message = {})
    {
        return {StatusCode::no_process, message};
    }
    [[nodiscard]] static constexpr Status exec_format(std::string_view message = {})
    {
        return {StatusCode::exec_format, message};
    }
    [[nodiscard]] static constexpr Status argument_list_too_long(std::string_view message = {})
    {
        return {StatusCode::argument_list_too_long, message};
    }
    [[nodiscard]] static constexpr Status no_memory(std::string_view message = {})
    {
        return {StatusCode::no_memory, message};
    }
    [[nodiscard]] static constexpr Status not_found(std::string_view message = {})
    {
        return {StatusCode::not_found, message};
    }
    [[nodiscard]] static constexpr Status already_exists(std::string_view message = {})
    {
        return {StatusCode::already_exists, message};
    }
    [[nodiscard]] static constexpr Status busy(std::string_view message = {})
    {
        return {StatusCode::busy, message};
    }
    [[nodiscard]] static constexpr Status denied(std::string_view message = {})
    {
        return {StatusCode::denied, message};
    }
    [[nodiscard]] static constexpr Status unsupported(std::string_view message = {})
    {
        return {StatusCode::unsupported, message};
    }
    [[nodiscard]] static constexpr Status overflow(std::string_view message = {})
    {
        return {StatusCode::overflow, message};
    }
    [[nodiscard]] static constexpr Status would_block(std::string_view message = {})
    {
        return {StatusCode::would_block, message};
    }
    [[nodiscard]] static constexpr Status interrupted(std::string_view message = {})
    {
        return {StatusCode::interrupted, message};
    }
    [[nodiscard]] static constexpr Status fault(std::string_view message = {})
    {
        return {StatusCode::fault, message};
    }
    [[nodiscard]] static constexpr Status not_initialized(std::string_view message = {})
    {
        return {StatusCode::not_initialized, message};
    }

    [[nodiscard]] constexpr bool ok() const
    {
        return code_ == StatusCode::ok;
    }
    [[nodiscard]] constexpr StatusCode code() const
    {
        return code_;
    }
    [[nodiscard]] constexpr std::string_view message() const
    {
        return message_;
    }
    [[nodiscard]] constexpr explicit operator bool() const
    {
        return ok();
    }

  private:
    StatusCode code_{StatusCode::ok};
    std::string_view message_{};
};

template <typename T> class Result final
{
  public:
    explicit Result(std::in_place_t) requires std::default_initializable<T>
        : value_(std::in_place), status_(Status::success())
    {
    }
    Result(T value) : value_(std::move(value)), status_(Status::success())
    {
    }
    Result(Status status) : value_(std::nullopt), status_(status)
    {
    }

    [[nodiscard]] static Result failure(Status status)
    {
        return Result(status);
    }

    [[nodiscard]] bool ok() const
    {
        return status_.ok();
    }
    [[nodiscard]] Status status() const
    {
        return status_;
    }
    [[nodiscard]] explicit operator bool() const
    {
        return ok();
    }

    [[nodiscard]] T &value() &
    {
        return *value_;
    }
    [[nodiscard]] const T &value() const &
    {
        return *value_;
    }
    [[nodiscard]] T &&value() &&
    {
        return std::move(*value_);
    }

  private:
    std::optional<T> value_;
    Status status_;
};

class NonCopyable
{
  protected:
    constexpr NonCopyable() = default;
    ~NonCopyable() = default;

  public:
    NonCopyable(const NonCopyable &) = delete;
    NonCopyable &operator=(const NonCopyable &) = delete;
};

} // namespace ok
