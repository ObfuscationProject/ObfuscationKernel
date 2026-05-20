#pragma once

#include "ok/core/kernel.hpp"

namespace ok
{

using KernelDebugWrite = void (*)(void *context, std::string_view text);

struct KernelDebugSink
{
    void *context{};
    KernelDebugWrite write{};

    void emit(std::string_view text) const
    {
        if (write != nullptr)
        {
            write(context, text);
        }
    }
};

enum class KernelBootMode : u8
{
    normal,
    debug,
};

struct KernelEntryConfig
{
    KernelBootMode mode{KernelBootMode::normal};
    KernelConfig kernel{};
    KernelDebugSink debug{};
};

struct KernelEntryResult
{
    Status status{};
    KernelTestReport tests{};
    usize debug_test_points{};
    u64 display_checksum{};
};

Status ok_kernel_entry(const KernelEntryConfig &config, KernelEntryResult *result = nullptr);
Result<std::string_view> ok_debug_shell_execute(std::string_view command);

extern "C" int ok_kernel_main(const KernelEntryConfig *config);

} // namespace ok
