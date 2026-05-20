#include "ok/core/entry.hpp"

namespace ok
{
namespace
{

Kernel &kernel_instance()
{
    static Kernel kernel;
    return kernel;
}

void emit(const KernelDebugSink &sink, std::string_view text)
{
    sink.emit(text);
}

void emit_unsigned(const KernelDebugSink &sink, u64 value)
{
    constexpr u64 powers[] = {
        10'000'000'000'000'000'000ull,
        1'000'000'000'000'000'000ull,
        100'000'000'000'000'000ull,
        10'000'000'000'000'000ull,
        1'000'000'000'000'000ull,
        100'000'000'000'000ull,
        10'000'000'000'000ull,
        1'000'000'000'000ull,
        100'000'000'000ull,
        10'000'000'000ull,
        1'000'000'000ull,
        100'000'000ull,
        10'000'000ull,
        1'000'000ull,
        100'000ull,
        10'000ull,
        1'000ull,
        100ull,
        10ull,
        1ull,
    };
    bool started = false;
    for (const auto power : powers)
    {
        u8 digit = 0;
        while (value >= power)
        {
            value -= power;
            ++digit;
        }
        if (digit != 0 || started || power == 1)
        {
            const char out = static_cast<char>('0' + digit);
            emit(sink, std::string_view{&out, 1});
            started = true;
        }
    }
}

void emit_bool_field(const KernelDebugSink &sink, std::string_view name, bool value)
{
    emit(sink, " ");
    emit(sink, name);
    emit(sink, "=");
    emit(sink, value ? "1" : "0");
}

void emit_unsigned_field(const KernelDebugSink &sink, std::string_view name, u64 value)
{
    emit(sink, " ");
    emit(sink, name);
    emit(sink, "=");
    emit_unsigned(sink, value);
}

void emit_failure(const KernelDebugSink &sink, Status status)
{
    emit(sink, "OK_TEST_FAIL code=");
    emit_unsigned(sink, static_cast<u32>(status.code()));
    emit(sink, " message=");
    emit(sink, status.message());
    emit(sink, "\n");
}

void emit_display_text(const KernelDebugSink &sink, std::string_view text)
{
    usize line_start = 0;
    for (usize i = 0; i <= text.size(); ++i)
    {
        if (i == text.size() || text[i] == '\n')
        {
            if (i > line_start)
            {
                emit(sink, "OK_DISPLAY_TEXT ");
                emit(sink, text.substr(line_start, i - line_start));
                emit(sink, "\n");
            }
            line_start = i + 1;
        }
    }
}

[[maybe_unused]] void emit_pass(const KernelDebugSink &sink, Kernel &kernel)
{
    const auto &report = kernel.test_report();
    emit_display_text(sink, kernel.display().text());
    emit(sink, "OK_TEST_PASS arch=");
    emit(sink, arch::to_string(kernel.arch().architecture()));
    emit_unsigned_field(sink, "processes", kernel.scheduler().process_count());
    emit_unsigned_field(sink, "cpus", kernel.topology().online_count());
    emit_unsigned_field(sink, "drivers", kernel.drivers().driver_count());
    emit_unsigned_field(sink, "free_frames", kernel.memory().frames().free_frames());
    emit_unsigned_field(sink, "syscalls", kernel.syscalls().handler_count());
    emit_unsigned_field(sink, "debug_test_points", kernel.debug_test_points_run());
    emit_bool_field(sink, "fs", report.vfs);
    emit_bool_field(sink, "simplefs", report.simplefs);
    emit_bool_field(sink, "ext4", report.ext4);
    emit_bool_field(sink, "user", report.user_mode);
    emit_bool_field(sink, "display", report.display);
    emit_bool_field(sink, "input", report.input);
    emit_bool_field(sink, "posix", report.posix);
    emit_bool_field(sink, "bus", report.bus);
    emit_bool_field(sink, "usb", report.usb);
    emit_bool_field(sink, "shell", report.shell);
    emit_bool_field(sink, "modes", report.modes);
    emit_unsigned_field(sink, "display_checksum", kernel.display().checksum());
    emit(sink, "\n");
}

} // namespace

Status ok_kernel_entry(const KernelEntryConfig &config, KernelEntryResult *result)
{
    const auto &sink = config.debug;

    if (config.mode == KernelBootMode::normal)
    {
        Kernel &kernel = kernel_instance();
        const auto status = kernel.boot(config.kernel);
        if (result != nullptr)
        {
            result->status = status;
            if (status.ok())
            {
                result->tests = kernel.test_report();
                result->display_checksum = kernel.display().checksum();
            }
        }
        return status;
    }

    emit(sink, "OK_MODE debug\n");

#if !defined(OK_ENABLE_TEST_POINTS)
    const auto status = Status::invalid_argument("kernel test entry requires a debug build with OK_ENABLE_TEST_POINTS");
    emit_failure(sink, status);
    if (result != nullptr)
    {
        result->status = status;
    }
    return status;
#else
    Kernel &kernel = kernel_instance();
    emit(sink, "OK_DEBUG boot=begin\n");
    if (auto status = kernel.boot(config.kernel); !status.ok())
    {
        emit_failure(sink, status);
        if (result != nullptr)
        {
            result->status = status;
        }
        return status;
    }
    emit(sink, "OK_DEBUG boot=complete\n");

    if (auto status = kernel.run_debug_test_suite(); !status.ok())
    {
        emit_failure(sink, status);
        if (result != nullptr)
        {
            result->status = status;
        }
        return status;
    }

    emit(sink, "OK_DEBUG fs=pass user=pass display=pass\n");
    emit_pass(sink, kernel);

    if (result != nullptr)
    {
        result->status = Status::success();
        result->tests = kernel.test_report();
        result->debug_test_points = kernel.debug_test_points_run();
        result->display_checksum = kernel.display().checksum();
    }
    return Status::success();
#endif
}

Result<std::string_view> ok_debug_shell_execute(std::string_view command)
{
    Kernel &kernel = kernel_instance();
    if (!kernel.booted())
    {
        return Status::not_initialized("kernel is not booted");
    }
    return kernel.debug_shell().execute(command);
}

extern "C" int ok_kernel_main(const KernelEntryConfig *config)
{
    if (config == nullptr)
    {
        KernelEntryConfig fallback{};
        fallback.mode = KernelBootMode::debug;
        return ok_kernel_entry(fallback).ok() ? 0 : 1;
    }
    return ok_kernel_entry(*config).ok() ? 0 : 1;
}

} // namespace ok
