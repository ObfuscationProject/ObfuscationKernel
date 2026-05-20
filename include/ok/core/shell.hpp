#pragma once

#include "ok/core/fixed.hpp"
#include "ok/core/types.hpp"

#include <string_view>

namespace ok
{

class Kernel;

class KernelDebugShell final
{
  public:
    Status attach(Kernel &kernel);
    Result<std::string_view> execute(std::string_view line);

  private:
    Status append(std::string_view text);
    Status append_unsigned(u64 value);
    Status command_help();
    Status command_status();
    Status command_memory();
    Status command_processes();
    Status command_drivers();
    Status command_filesystem();
    Status command_posix();
    Status command_tests();
    Status command_echo(std::string_view text);

    Kernel *kernel_{nullptr};
    FixedString<768> output_{};
};

} // namespace ok
