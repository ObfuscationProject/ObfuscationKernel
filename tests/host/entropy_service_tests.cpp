#include "ok/posix/posix.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace
{
using namespace ok;

void require(bool condition, const char *message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL entropy service: %s\n", message);
        std::exit(1);
    }
}

class FixtureSource final : public driver::EntropySource
{
  public:
    Status fill(std::span<std::byte> out) override
    {
        ++calls;
        if (fail_on == calls)
        {
            // Deliberately violate the provider's atomicity promise. The
            // kernel's staging buffer must still protect caller output.
            if (!out.empty()) out[0] = std::byte{0xff};
            return Status::would_block("fixture source unavailable");
        }
        for (auto &byte : out) byte = std::byte{static_cast<u8>(++sequence)};
        return Status::success();
    }
    usize calls{};
    usize fail_on{};
    usize sequence{};
};
}

int main()
{
    posix::PosixService service;
    std::array<std::byte, 600> output{};
    output.fill(std::byte{0xa5});
    auto absent = service.getrandom(output);
    require(!absent && absent.status().code() == StatusCode::would_block,
            "missing source must return EAGAIN state");
    for (auto byte : output) require(byte == std::byte{0xa5}, "missing source changed output");
    auto empty = service.getrandom(std::span<std::byte>{});
    require(empty && empty.value() == 0, "empty request must succeed");
    auto invalid = service.getrandom(output, 2);
    require(!invalid && invalid.status().code() == StatusCode::invalid_argument,
            "unsupported GRND_RANDOM flag must fail");

    FixtureSource source;
    service.set_entropy_source(&source);
    auto filled = service.getrandom(output, 1);
    require(filled && filled.value() == output.size() && source.calls == 3,
            "full request must consume three bounded device chunks");
    for (usize index = 0; index < output.size(); ++index)
        require(output[index] == std::byte{static_cast<u8>(index + 1)}, "device bytes were not copied");

    output.fill(std::byte{0xa5});
    source.calls = 0;
    source.fail_on = 1;
    auto failed = service.getrandom(output);
    require(!failed && failed.status().code() == StatusCode::would_block,
            "first failed device chunk must fail request");
    for (auto byte : output) require(byte == std::byte{0xa5}, "failed chunk exposed device bytes");

    source.calls = 0;
    source.fail_on = 2;
    auto partial = service.getrandom(output);
    require(partial && partial.value() == 256, "second failed chunk must report committed prefix");
    for (usize index = 256; index < output.size(); ++index)
        require(output[index] == std::byte{0xa5}, "failed later chunk changed uncommitted suffix");

    service.set_entropy_source(nullptr);
    require(!service.getrandom(output), "removed source must fail closed");
    std::puts("PASS entropy service: absent, flags, success, atomic failure, partial prefix");
}
