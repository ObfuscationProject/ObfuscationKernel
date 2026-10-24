#include "ok/posix/posix.hpp"

namespace ok::posix
{

Result<usize> PosixService::getrandom(std::span<std::byte> out, u32 flags)
{
    constexpr u32 grnd_nonblock = 1;
    if (flags & ~grnd_nonblock)
        return Status::invalid_argument("unsupported getrandom flags");
    if (out.empty())
        return usize{0};
    if (!entropy_source_)
        return Status::would_block("trusted entropy source is unavailable");

    // The source receives only private staging memory. A failure after an
    // earlier successful chunk returns that committed prefix, as Linux does.
    std::array<std::byte, 256> staging{};
    usize complete = 0;
    while (complete < out.size())
    {
        const usize count = out.size() - complete < staging.size() ? out.size() - complete : staging.size();
        if (auto status = entropy_source_->fill({staging.data(), count}); !status)
            return complete ? Result<usize>{complete} : Result<usize>{status};
        for (usize i = 0; i < count; ++i)
            out[complete + i] = staging[i];
        complete += count;
    }
    return complete;
}

} // namespace ok::posix
