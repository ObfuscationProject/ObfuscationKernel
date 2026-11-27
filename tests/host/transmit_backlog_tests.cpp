// The datagram transmit backlog (smp-scaling WP5, spec 4.13 try-transmit):
// frames the NIC refuses with would_block wait in order, a full backlog
// refuses more (the send's ENOBUFS), a flush stops at the first would_block
// and drops (counts) a frame the NIC refuses otherwise. scope=model.
#include "ok/net/transmit_backlog.hpp"

#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

using namespace ok;

#define CHECK(condition)                                                                                              \
    do                                                                                                                \
    {                                                                                                                 \
        if (!static_cast<bool>(condition))                                                                            \
        {                                                                                                             \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                                      \
            std::abort();                                                                                             \
        }                                                                                                             \
    } while (false)

int main()
{
    net::TransmitBacklog<3, 16> backlog;
    const auto frame = [](u8 tag, usize size) {
        std::vector<std::byte> bytes(size, std::byte{tag});
        return bytes;
    };
    CHECK(backlog.empty() && !backlog.full());
    CHECK(!backlog.push({}) && backlog.push(frame(9, 17)).code() == StatusCode::invalid_argument);
    for (u8 tag = 1; tag <= 3; ++tag)
    {
        const auto bytes = frame(tag, 4 + tag);
        CHECK(backlog.push(bytes));
    }
    CHECK(backlog.full());
    const auto overflow = frame(4, 8);
    const auto refused = backlog.push(overflow);
    CHECK(!refused && refused.code() == StatusCode::no_space && backlog.refused() == 1);
    // The NIC takes one frame, then would block: the rest stay, in order.
    std::vector<u8> sent;
    usize credit = 1;
    CHECK(!backlog.flush([&](std::span<const std::byte> bytes) -> Status {
        if (!credit) return Status::would_block("ring full");
        --credit;
        sent.push_back(std::to_integer<u8>(bytes[0]));
        CHECK(bytes.size() == 4u + sent.back());
        return {};
    }));
    CHECK(sent == std::vector<u8>{1} && backlog.size() == 2);
    // A frame the NIC refuses for good is dropped; the next one is sent.
    bool first = true;
    CHECK(backlog.flush([&](std::span<const std::byte> bytes) -> Status {
        if (std::exchange(first, false)) return Status::io_error("link down");
        sent.push_back(std::to_integer<u8>(bytes[0]));
        return {};
    }));
    CHECK(sent == (std::vector<u8>{1, 3}) && backlog.empty() && backlog.dropped() == 1 && backlog.queued() == 3);
    CHECK(backlog.flushed() == 2 && backlog.direct() == 0);
    backlog.count_direct();
    CHECK(backlog.direct() == 1);
    // Wrap-around keeps the order.
    for (u8 round = 0; round < 10; ++round)
    {
        const auto a = frame(static_cast<u8>(10 + round), 5), b = frame(static_cast<u8>(40 + round), 6);
        CHECK(backlog.push(a) && backlog.push(b));
        std::vector<u8> order;
        CHECK(backlog.flush([&](std::span<const std::byte> bytes) -> Status {
            order.push_back(std::to_integer<u8>(bytes[0]));
            return {};
        }));
        CHECK(order == (std::vector<u8>{static_cast<u8>(10 + round), static_cast<u8>(40 + round)}));
    }
    std::puts("OK_TRANSMIT_BACKLOG scope=model pass");
    return 0;
}
