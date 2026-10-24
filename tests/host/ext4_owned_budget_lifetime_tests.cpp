// Compile with -fno-access-control for serialized rejected-dispatch injection.
#include "ok/fs/ext4_owned_transaction.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string_view>

using namespace ok;
using namespace ok::fs;

namespace
{
usize checks{}, cases{};
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "owned budget lifetime:%d: %s\n", __LINE__, #expression); \
    std::abort(); } } while (false)

class Disk final : public driver::BlockDevice
{
  public:
    driver::BlockGeometry geometry() const override { return {65536, 512, true}; }
    Status read_blocks(u64, std::span<std::byte> out) override
    {
        ++reads;
        for (auto &byte : out) byte = std::byte{0x34};
        return media_failure ? Status::io_error("owned lifetime injected media failure") : Status{};
    }
    Status write_blocks(u64, std::span<const std::byte>) override
    { ++writes; return Status::fault("owned lifetime unexpected write"); }
    Status flush() override
    { ++flushes; return Status::fault("owned lifetime unexpected flush"); }
    usize reads{}, writes{}, flushes{};
    bool media_failure{};
};

using Owner = Ext4OwnedTransaction<1, 8>;
using Registry = Owner::Registry;

void rejected_finish(bool media_failure)
{
    Disk disk;
    disk.media_failure = media_failure;
    Registry registry;
    auto registration = registry.register_device(disk, media_failure ? 34002 : 34001);
    CHECK(registration);
    auto owner = std::make_unique<Owner>();
    CHECK(owner->open(registry, registration.value(), {0, 65536}));
    auto &session = owner->cache_.session_;
    auto &budget = owner->startup_route_budget_;
    CHECK(budget.reserve_opening(session));
    CHECK(budget.begin_read(session, 2));
    std::array<std::byte, 1024> bytes{};
    const auto transport = session.read_blocks(2, bytes);
    CHECK(bool(transport) == !media_failure);
    budget.control_.transfer_bytes ^= 512;
    CHECK(budget.finish_read(session, transport).code() == StatusCode::fault);
    CHECK(budget.report().dispatch_pending);
    const auto before = registry.inspect(registration.value());
    CHECK(before && before.value().state == block::DeviceState::quarantined);
    CHECK(before.value().first_error == (media_failure ? StatusCode::io_error : StatusCode::fault));
    CHECK(before.value().reads == 1 && before.value().completed_reads == usize(!media_failure));
    // This pending Budget must retire while the real embedded Session is alive.
    owner.reset();
    const auto after = registry.inspect(registration.value());
    CHECK(after && reclaim_history::same_session(before.value(), after.value()));
    CHECK(disk.reads == 1 && !disk.writes && !disk.flushes);
    ++cases;
}

void pending_without_dispatch()
{
    Disk disk;
    Registry registry;
    auto registration = registry.register_device(disk, 34003); CHECK(registration);
    auto owner = std::make_unique<Owner>();
    CHECK(owner->open(registry, registration.value(), {0, 65536}));
    CHECK(owner->startup_route_budget_.reserve_opening(owner->cache_.session_));
    CHECK(owner->startup_route_budget_.begin_read(owner->cache_.session_, 2));
    CHECK(owner->startup_route_budget_.report().dispatch_pending);
    owner.reset();
    const auto after = registry.inspect(registration.value());
    CHECK(after && after.value().state == block::DeviceState::quarantined);
    CHECK(after.value().first_error == StatusCode::interrupted && !after.value().reads);
    CHECK(!disk.reads && !disk.writes && !disk.flushes);
    ++cases;
}

void successful_scope(bool close_owner)
{
    Disk disk;
    Registry registry;
    auto registration = registry.register_device(disk, close_owner ? 34005 : 34004); CHECK(registration);
    auto owner = std::make_unique<Owner>();
    CHECK(owner->open(registry, registration.value(), {0, 65536}));
    auto &session = owner->cache_.session_;
    auto &budget = owner->startup_route_budget_;
    CHECK(budget.reserve_opening(session));
    std::array<std::byte, 1024> bytes{};
    CHECK(block::guarded_read_blocks(session, budget, 2, bytes));
    CHECK(budget.close(session));
    CHECK(!budget.report().dispatch_pending && budget.report().attempts[0] == 1);
    if (close_owner) CHECK(owner->close());
    owner.reset();
    const auto after = registry.inspect(registration.value()); CHECK(after);
    CHECK(after.value().state == (close_owner ? block::DeviceState::ready : block::DeviceState::quarantined));
    CHECK(after.value().first_error == (close_owner ? StatusCode::ok : StatusCode::interrupted));
    CHECK(after.value().reads == 1 && after.value().completed_reads == 1);
    CHECK(!disk.writes && !disk.flushes);
    ++cases;
}

void external_scope()
{
    Disk disk;
    Registry registry;
    auto registration = registry.register_device(disk, 34006); CHECK(registration);
    Registry::Session session;
    CHECK(registry.acquire(registration.value(), {0, 65536}, block::SessionAccess::writable, session));
    {
        Ext4ReclaimRouteBudget<Registry::Session> budget;
        CHECK(budget.reserve_opening(session));
        CHECK(budget.begin_read(session, 2));
        // The caller's Session remains live when the external Guard destructs.
    }
    const auto after = registry.inspect(registration.value()); CHECK(after);
    CHECK(after.value().state == block::DeviceState::quarantined);
    CHECK(after.value().first_error == StatusCode::interrupted && !after.value().reads);
    CHECK(session.report() && !disk.reads && !disk.writes && !disk.flushes);
    ++cases;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::string_view{argv[1]} == "parent-negative")
        rejected_finish(false);
    else
    {
        CHECK(argc == 1);
        rejected_finish(false);
        rejected_finish(true);
        pending_without_dispatch();
        successful_scope(false);
        successful_scope(true);
        external_scope();
        CHECK(cases == 6);
    }
    std::printf("PASS owned budget lifetime: checks=%zu cases=%zu original quarantine, close and external borrow; zero writes/flushes\n",
                checks, cases);
}
