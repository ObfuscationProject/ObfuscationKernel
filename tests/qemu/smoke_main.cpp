#include "ok/arch/arch.hpp"
#include "ok/core/kernel.hpp"

#include <string_view>
#include <unistd.h>

namespace
{

void write_all(int fd, std::string_view text)
{
    const char *data = text.data();
    auto size = text.size();
    while (size != 0)
    {
        const auto written = ::write(fd, data, size);
        if (written <= 0)
        {
            return;
        }
        data += written;
        size -= static_cast<ok::usize>(written);
    }
}

void write_unsigned(int fd, ok::u64 value)
{
    char digits[20]{};
    ok::usize count = 0;
    do
    {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0);
    while (count != 0)
    {
        --count;
        write_all(fd, std::string_view{&digits[count], 1});
    }
}

void write_field(int fd, std::string_view name, ok::u64 value)
{
    write_all(fd, " ");
    write_all(fd, name);
    write_all(fd, "=");
    write_unsigned(fd, value);
}

int fail(ok::Status status)
{
    write_all(STDERR_FILENO, "OK_TEST_FAIL code=");
    write_unsigned(STDERR_FILENO, static_cast<unsigned>(status.code()));
    write_all(STDERR_FILENO, " message=");
    write_all(STDERR_FILENO, status.message());
    write_all(STDERR_FILENO, "\n");
    return 1;
}

} // namespace

int main()
{
    ok::Kernel kernel;
    ok::KernelConfig config{};
    config.architecture = ok::arch::configured_architecture();
    const auto architecture = config.architecture;

    if (auto status = kernel.boot(config); !status.ok())
    {
        return fail(status);
    }
    if (auto status = kernel.run_smoke_suite(); !status.ok())
    {
        return fail(status);
    }

    write_all(STDOUT_FILENO, "OK_TEST_PASS arch=");
    write_all(STDOUT_FILENO, ok::arch::to_string(architecture));
    write_field(STDOUT_FILENO, "processes", kernel.scheduler().process_count());
    write_field(STDOUT_FILENO, "cpus", kernel.topology().online_count());
    write_field(STDOUT_FILENO, "drivers", kernel.drivers().driver_count());
    write_field(STDOUT_FILENO, "free_frames", kernel.memory().frames().free_frames());
    write_field(STDOUT_FILENO, "syscalls", kernel.syscalls().handler_count());
    write_field(STDOUT_FILENO, "debug_test_points", kernel.debug_test_points_run());
    write_all(STDOUT_FILENO, "\n");
    return 0;
}
