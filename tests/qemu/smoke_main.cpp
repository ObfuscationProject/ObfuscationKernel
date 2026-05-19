#include "ok/arch/arch.hpp"
#include "ok/core/entry.hpp"

#include <string_view>
#include <unistd.h>

namespace
{

void write_all(void *, std::string_view text)
{
    const char *data = text.data();
    auto size = text.size();
    while (size != 0)
    {
        const auto written = ::write(STDOUT_FILENO, data, size);
        if (written <= 0)
        {
            return;
        }
        data += written;
        size -= static_cast<ok::usize>(written);
    }
}

} // namespace

int main()
{
    ok::KernelEntryConfig config{};
    config.mode = ok::KernelBootMode::debug;
    config.kernel.architecture = ok::arch::configured_architecture();
    config.debug = ok::KernelDebugSink{.context = nullptr, .write = write_all};
    return ok_kernel_main(&config);
}
