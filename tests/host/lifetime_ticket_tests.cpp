#include "ok/fs/ext4/alloc.hpp"

#include <cstdio>
#include <cstdlib>

using namespace ok;
using namespace ok::fs;

#define CHECK(...) do { if (!(__VA_ARGS__)) { \
    std::fprintf(stderr, "lifetime ticket:%d: %s\n", __LINE__, #__VA_ARGS__); std::abort(); \
} } while (false)

namespace
{
using Table = Ext4InodeLifetimeTable<2, 4>;
usize accepted{}, rejected{};
void accept(Status status) { CHECK(status); ++accepted; }
void reject(Status status)
{ CHECK(!status && status.code() == StatusCode::bad_descriptor); ++rejected; }

void serials_and_copies()
{
    int mount{}, foreign_mount{};
    const Ext4InodeLifetimeKey key{&mount, 17, 12, 3};
    Table table{}, foreign{};
    Table::Ticket empty{}, ticket{};
    reject(table.validate_dispatched(empty, key));
    CHECK(table.observe(key)); CHECK(table.mark_orphan(key));
    CHECK(table.schedule_one([&](const Table::Ticket &issued) -> Status {
        ticket = issued;
        // The callback is outside the table lock; validation can re-enter it.
        accept(table.validate_dispatched(issued, key));
        return {};
    }));
    const auto copy = ticket;
    accept(table.validate_dispatched(copy, key));
    CHECK(table.active_records() == 1);
    reject(foreign.validate_dispatched(copy, key));
    reject(table.validate_dispatched(copy, {&foreign_mount, 17, 12, 3}));
    reject(table.validate_dispatched(copy, {&mount, 18, 12, 3}));
    reject(table.validate_dispatched(copy, {&mount, 17, 13, 3}));
    reject(table.validate_dispatched(copy, {&mount, 17, 12, 4}));
    reject(table.validate_dispatched(copy, {}));
    CHECK(table.retry(ticket));
    reject(table.validate_dispatched(ticket, key));
    reject(table.validate_dispatched(copy, key));
    CHECK(table.schedule_one([&](const Table::Ticket &issued) -> Status {
        ticket = issued; return {};
    }));
    accept(table.validate_dispatched(ticket, key));
    reject(table.validate_dispatched(copy, key));
    // Public keys are identical across dispatches; old serials must still fail.
    CHECK(copy.key() == ticket.key());
    CHECK(table.acknowledge_reclaimed(ticket));
    reject(table.validate_dispatched(ticket, key));
    CHECK(table.active_records() == 0);
    // Re-observing even the exact old disk key obtains a new record serial.
    CHECK(table.observe(key)); CHECK(table.mark_orphan(key));
    Table::Ticket fresh{};
    CHECK(table.schedule_one([&](const Table::Ticket &issued) -> Status {
        fresh = issued; return {};
    }));
    accept(table.validate_dispatched(fresh, key));
    reject(table.validate_dispatched(ticket, key));
    reject(table.validate_dispatched(copy, key));
    CHECK(table.snapshot(key).value().phase == Ext4InodeLifetimePhase::dispatched);
    CHECK(table.active_records() == 1); // Validation never acknowledges a record.
    CHECK(table.acknowledge_reclaimed(fresh));
}

void enqueue_failure_and_occupied_leases()
{
    int mount{};
    const Ext4InodeLifetimeKey key{&mount, 5, 14, 0};
    Table table{};
    CHECK(table.observe(key));
    auto fd = table.retain(key, Ext4InodeHold::descriptor); CHECK(fd);
    auto mapping = table.clone(fd.value(), Ext4InodeHold::mapping); CHECK(mapping);
    auto io = table.clone(fd.value(), Ext4InodeHold::io); CHECK(io);
    auto pin = table.clone(fd.value(), Ext4InodeHold::pin); CHECK(pin);
    CHECK(table.mark_orphan(key));
    usize calls{};
    const auto cannot_schedule = [&] {
        const auto status = table.schedule_one([&](const Table::Ticket &) -> Status {
            ++calls; return Status::fault("occupied inode must not dispatch");
        });
        CHECK(!status && status.code() == StatusCode::not_found && !calls);
        reject(table.validate_dispatched(Table::Ticket{}, key));
    };
    cannot_schedule(); CHECK(table.release(fd.value()));
    cannot_schedule(); CHECK(table.release(mapping.value()));
    cannot_schedule(); CHECK(table.release(io.value()));
    cannot_schedule(); CHECK(table.release(pin.value()));
    Table::Ticket failed{};
    CHECK(!table.schedule_one([&](const Table::Ticket &issued) -> Status {
        failed = issued; accept(table.validate_dispatched(issued, key));
        return Status::no_space("queue unavailable");
    }));
    reject(table.validate_dispatched(failed, key));
    Table::Ticket current{};
    CHECK(table.schedule_one([&](const Table::Ticket &issued) -> Status {
        current = issued; return {};
    }));
    reject(table.validate_dispatched(failed, key));
    accept(table.validate_dispatched(current, key));
    // Test-only -fno-access-control injects an impossible occupied dispatch.
    // Exercise each defensive zero-hold check without claiming API reachability.
    for (usize kind = 0; kind < 4; ++kind)
    {
        table.records_[0].holds[kind] = 1;
        reject(table.validate_dispatched(current, key));
        table.records_[0].holds[kind] = 0;
        accept(table.validate_dispatched(current, key));
    }
    // Counter sums are not a zero-hold proof: both arrays wrap u32 total to 0.
    table.records_[0].holds = {0x80000000U, 0x80000000U, 0, 0};
    reject(table.validate_dispatched(current, key));
    table.records_[0].holds = {0xffffffffU, 1, 0, 0};
    reject(table.validate_dispatched(current, key));
    table.records_[0].holds = {};
    CHECK(table.acknowledge_reclaimed(current));
}
}

int main()
{
    serials_and_copies(); enqueue_failure_and_occupied_leases();
    std::printf("PASS lifetime dispatched validation: accepted=%zu rejected=%zu serial/copy/retry/reuse/foreign/occupied; no automatic acknowledgement\n",
                accepted, rejected);
}
