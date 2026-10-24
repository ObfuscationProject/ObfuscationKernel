#include "ok/fs/ext4_inode_lifetime.hpp"

#include <cstdio>
#include <cstdlib>

using namespace ok;
using namespace ok::fs;

#define CHECK(...) do { if (!(__VA_ARGS__)) { \
    std::fprintf(stderr, "lifetime exact:%d: %s\n", __LINE__, #__VA_ARGS__); std::abort(); \
} } while (false)

namespace
{
using Table = Ext4InodeLifetimeTable<4, 8>;
usize callbacks{}, rejected{}, stale{};

void reject_schedule(Table &table, Ext4InodeLifetimeKey key, StatusCode code)
{
    const auto before = callbacks;
    const auto status = table.schedule_exact(key, [&](const Table::Ticket &) -> Status {
        ++callbacks; return {};
    });
    CHECK(!status && status.code() == code && callbacks == before); ++rejected;
}
void reject_ticket(const Table &table, const Table::Ticket &ticket,
                   Ext4InodeLifetimeKey key)
{
    const auto status = table.validate_dispatched(ticket, key);
    CHECK(!status && status.code() == StatusCode::bad_descriptor); ++stale;
}
void make_ready(Table &table, Ext4InodeLifetimeKey key)
{ CHECK(table.observe(key)); CHECK(table.mark_orphan(key)); }

void exact_head_and_phases()
{
    int mount{}, wrong_mount{};
    const Ext4InodeLifetimeKey nonhead{&mount, 9, 12, 4}, head{&mount, 9, 14, 0};
    Table table{}, other{};
    make_ready(table, nonhead); CHECK(table.observe(head));
    reject_schedule(table, {nullptr, 9, 14, 0}, StatusCode::invalid_argument);
    reject_schedule(table, {&mount, 0, 14, 0}, StatusCode::invalid_argument);
    reject_schedule(table, {&mount, 9, 0, 0}, StatusCode::invalid_argument);
    reject_schedule(table, {&wrong_mount, 9, 14, 0}, StatusCode::not_found);
    reject_schedule(table, {&mount, 10, 14, 0}, StatusCode::not_found);
    reject_schedule(table, {&mount, 9, 15, 0}, StatusCode::not_found);
    reject_schedule(table, {&mount, 9, 14, 1}, StatusCode::not_found);
    reject_schedule(other, head, StatusCode::not_found);
    reject_schedule(table, head, StatusCode::busy); // Linked without holds.
    auto fd = table.retain(head, Ext4InodeHold::descriptor); CHECK(fd);
    auto io = table.clone(fd.value(), Ext4InodeHold::io); CHECK(io);
    reject_schedule(table, head, StatusCode::busy); // Linked with holds.
    CHECK(table.mark_orphan(head));
    reject_schedule(table, head, StatusCode::busy); // Retired; nonhead is ready.
    CHECK(table.snapshot(nonhead).value().phase == Ext4InodeLifetimePhase::ready);
    CHECK(table.release(io.value())); reject_schedule(table, head, StatusCode::busy);
    CHECK(table.release(fd.value()));
    Table::Ticket head_ticket{}, nonhead_ticket{};
    CHECK(table.schedule_exact(head, [&](const Table::Ticket &issued) -> Status {
        ++callbacks; head_ticket = issued;
        CHECK(issued.key() == head && table.validate_dispatched(issued, head));
        // Callback is outside the table lock; all checks can re-enter it.
        CHECK(table.snapshot(head).value().phase == Ext4InodeLifetimePhase::dispatched);
        CHECK(table.snapshot(nonhead).value().phase == Ext4InodeLifetimePhase::ready);
        return {};
    }));
    reject_schedule(table, head, StatusCode::busy); // Already dispatched.
    CHECK(table.schedule_one([&](const Table::Ticket &issued) -> Status {
        ++callbacks; nonhead_ticket = issued; CHECK(issued.key() == nonhead); return {};
    })); // Legacy first-ready selection remains useful and unchanged.
    const auto old = head_ticket;
    CHECK(table.retry(head_ticket)); reject_ticket(table, old, head);
    CHECK(table.schedule_exact(head, [&](const Table::Ticket &issued) -> Status {
        ++callbacks; head_ticket = issued; return {};
    }));
    reject_ticket(table, old, head); CHECK(table.validate_dispatched(head_ticket, head));
    CHECK(table.acknowledge_reclaimed(head_ticket)); reject_ticket(table, head_ticket, head);
    make_ready(table, head);
    CHECK(table.schedule_exact(head, [&](const Table::Ticket &issued) -> Status {
        ++callbacks; head_ticket = issued; return {};
    }));
    reject_ticket(table, old, head); CHECK(table.acknowledge_reclaimed(head_ticket));
    CHECK(table.acknowledge_reclaimed(nonhead_ticket)); CHECK(table.active_records() == 0);
}

void failure_and_nested_serials()
{
    int mount{};
    const Ext4InodeLifetimeKey key{&mount, 5, 20, 2};
    Table table{}; make_ready(table, key);
    Table::Ticket failed{}, current{}, outer{};
    const auto first = table.schedule_exact(key, [&](const Table::Ticket &issued) -> Status {
        ++callbacks; failed = issued; CHECK(table.validate_dispatched(issued, key));
        return Status::no_space("enqueue unavailable");
    });
    CHECK(!first && first.code() == StatusCode::no_space);
    CHECK(table.snapshot(key).value().phase == Ext4InodeLifetimePhase::ready);
    reject_ticket(table, failed, key);
    CHECK(table.schedule_exact(key, [&](const Table::Ticket &issued) -> Status {
        ++callbacks; current = issued; return {};
    }));
    reject_ticket(table, failed, key); CHECK(table.retry(current));
    const auto nested = table.schedule_exact(key, [&](const Table::Ticket &issued) -> Status {
        ++callbacks; outer = issued; CHECK(table.retry(issued));
        CHECK(table.schedule_exact(key, [&](const Table::Ticket &inner) -> Status {
            ++callbacks; current = inner; CHECK(table.validate_dispatched(inner, key)); return {};
        }));
        return Status::no_space("older callback failed after redispatch");
    });
    CHECK(!nested && nested.code() == StatusCode::no_space);
    reject_ticket(table, outer, key); CHECK(table.validate_dispatched(current, key));
    CHECK(table.snapshot(key).value().phase == Ext4InodeLifetimePhase::dispatched);
    CHECK(table.acknowledge_reclaimed(current));

    // Identical public key with a NEW record serial must not be rolled back by
    // the old callback, even if the old dispatch completed synchronously.
    make_ready(table, key);
    const auto reused = table.schedule_exact(key, [&](const Table::Ticket &issued) -> Status {
        ++callbacks; outer = issued; CHECK(table.acknowledge_reclaimed(issued));
        make_ready(table, key);
        CHECK(table.schedule_exact(key, [&](const Table::Ticket &inner) -> Status {
            ++callbacks; current = inner; return {};
        }));
        return Status::no_space("old record callback failure");
    });
    CHECK(!reused && reused.code() == StatusCode::no_space);
    reject_ticket(table, outer, key); CHECK(table.validate_dispatched(current, key));
    CHECK(table.snapshot(key).value().phase == Ext4InodeLifetimePhase::dispatched);
    CHECK(table.acknowledge_reclaimed(current));
}

void defensive_overflow_and_holds()
{
    int mount{};
    const Ext4InodeLifetimeKey key{&mount, 6, 25, 3};
    Table table{}; make_ready(table, key);
    // Test-only -fno-access-control injects serial exhaustion without billions
    // of operations. Rejection leaves phase and ticket identity untouched.
    table.dispatch_serial_ = ~u64{0};
    reject_schedule(table, key, StatusCode::overflow);
    CHECK(table.snapshot(key).value().phase == Ext4InodeLifetimePhase::ready);
    CHECK(table.records_[0].dispatch_serial == 0);
    table.dispatch_serial_ = ~u64{0} - 1;
    Table::Ticket last{};
    CHECK(table.schedule_exact(key, [&](const Table::Ticket &issued) -> Status {
        ++callbacks; last = issued; CHECK(table.validate_dispatched(issued, key)); return {};
    }));
    CHECK(table.dispatch_serial_ == ~u64{0}); CHECK(table.retry(last));
    reject_schedule(table, key, StatusCode::overflow); reject_ticket(table, last, key);
    table.dispatch_serial_ = 0;
    // Impossible ready-with-holds corruption must not pass through sum wrap.
    for (usize kind = 0; kind < 4; ++kind)
    {
        table.records_[0].holds[kind] = 1;
        reject_schedule(table, key, StatusCode::fault);
        table.records_[0].holds[kind] = 0;
    }
    table.records_[0].holds = {0x80000000U, 0x80000000U, 0, 0};
    reject_schedule(table, key, StatusCode::fault);
    table.records_[0].holds = {};
    CHECK(table.snapshot(key).value().phase == Ext4InodeLifetimePhase::ready);
}
}

int main()
{
    exact_head_and_phases(); failure_and_nested_serials(); defensive_overflow_and_holds();
    CHECK(callbacks == 11 && rejected == 20 && stale == 9);
    std::printf("PASS exact lifetime scheduler: callbacks=%zu rejected=%zu stale=%zu exact_head legacy reentrancy rollback serial_overflow zero_holds; no automatic acknowledgement\n",
                callbacks, rejected, stale);
}
