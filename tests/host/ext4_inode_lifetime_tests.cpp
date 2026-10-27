#include "ok/fs/ext4/alloc.hpp"
#include "ok/fs/vfs.hpp"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <type_traits>
#include <utility>

using namespace ok;
using namespace ok::fs;

#define CHECK(...) do { if (!(__VA_ARGS__)) { \
    std::fprintf(stderr, "inode lifetime model:%d: %s\n", __LINE__, #__VA_ARGS__); std::abort(); \
} } while (false)

namespace
{
using Table = Ext4InodeLifetimeTable<4, 8>;
static_assert(!std::is_copy_constructible_v<Table::Lease> &&
              !std::is_copy_assignable_v<Table::Lease> &&
              std::is_move_constructible_v<Table::Lease> &&
              !std::is_move_assignable_v<Table::Lease>);

void check_status(Status status, StatusCode code)
{ CHECK(!status && status.code() == code); }

template <typename T>
void check_result(const Result<T> &result, StatusCode code)
{ CHECK(!result && result.status().code() == code); }

void lifecycle()
{
    int mount{};
    int other_mount{};
    const Ext4InodeLifetimeKey key{&mount, 7, 12, 3};
    Table table{};
    CHECK(table.observe(key));
    CHECK(table.observe(key));
    check_status(table.observe({nullptr, 7, 12, 3}), StatusCode::invalid_argument);
    check_status(table.observe({&mount, 0, 12, 3}), StatusCode::invalid_argument);
    check_status(table.observe({&mount, 7, 12, 4}), StatusCode::busy);
    CHECK(table.active_records() == 1);
    auto fd_result = table.retain(key, Ext4InodeHold::descriptor);
    CHECK(fd_result); auto fd = std::move(fd_result).value();
    CHECK(!fd_result.value().valid() && fd.valid());
    auto mapping_result = table.clone(fd, Ext4InodeHold::mapping);
    CHECK(mapping_result); auto mapping = std::move(mapping_result).value();
    auto io_result = table.clone(fd, Ext4InodeHold::io);
    CHECK(io_result); auto io = std::move(io_result).value();
    auto pin_result = table.clone(io, Ext4InodeHold::pin);
    CHECK(pin_result); auto pin = std::move(pin_result).value();
    auto snapshot = table.snapshot(key);
    CHECK(snapshot && snapshot.value().holds == std::array<u32, 4>{1, 1, 1, 1} &&
          snapshot.value().total() == 4 &&
          snapshot.value().phase == Ext4InodeLifetimePhase::linked);

    CHECK(table.mark_orphan(key));
    check_status(table.mark_orphan(key), StatusCode::already_exists);
    check_result(table.retain(key, Ext4InodeHold::descriptor), StatusCode::busy);
    check_status(table.observe({&mount, 8, 12, 4}), StatusCode::busy);
    auto duplicate_result = table.clone(fd, Ext4InodeHold::descriptor);
    CHECK(duplicate_result); auto duplicate = std::move(duplicate_result).value();
    snapshot = table.snapshot(key);
    CHECK(snapshot && snapshot.value().holds == std::array<u32, 4>{2, 1, 1, 1} &&
          snapshot.value().phase == Ext4InodeLifetimePhase::retired);
    CHECK(table.release(fd)); CHECK(table.release(duplicate));
    CHECK(table.release(mapping)); CHECK(table.release(io));
    CHECK(table.schedule_one([](const Table::Ticket &) -> Status {
        return Status::fault("must not schedule with a pin");
    }).code() == StatusCode::not_found);
    auto final_pin = std::move(pin);
    CHECK(!pin.valid() && final_pin.valid());
    check_status(table.release(pin), StatusCode::bad_descriptor);
    CHECK(table.release(final_pin));
    check_status(table.release(final_pin), StatusCode::bad_descriptor);
    snapshot = table.snapshot(key);
    CHECK(snapshot && snapshot.value().total() == 0 &&
          snapshot.value().phase == Ext4InodeLifetimePhase::ready);
    check_result(table.clone(final_pin, Ext4InodeHold::mapping), StatusCode::bad_descriptor);

    Table::Ticket first_ticket{};
    usize callbacks{};
    check_status(table.schedule_one([&](const Table::Ticket &ticket) -> Status {
        ++callbacks;
        first_ticket = ticket;
        // Re-entering the table proves the callback is outside its spinlock.
        auto checked = table.snapshot(key);
        CHECK(checked && checked.value().phase == Ext4InodeLifetimePhase::dispatched);
        CHECK(table.observe({&other_mount, 9, 15, 0}));
        return Status::no_space("worker queue full");
    }), StatusCode::no_space);
    snapshot = table.snapshot(key);
    CHECK(callbacks == 1 && snapshot && snapshot.value().phase == Ext4InodeLifetimePhase::ready);
    check_status(table.acknowledge_reclaimed(first_ticket), StatusCode::bad_descriptor);

    Table::Ticket queued{};
    CHECK(table.schedule_one([&](const Table::Ticket &ticket) -> Status {
        ++callbacks; queued = ticket;
        CHECK(ticket.key() == key);
        CHECK(table.active_records() == 2);
        return {};
    }));
    CHECK(callbacks == 2);
    check_status(table.acknowledge_reclaimed(first_ticket), StatusCode::bad_descriptor);
    check_result(table.retain(key, Ext4InodeHold::descriptor), StatusCode::busy);
    CHECK(table.retry(queued));
    check_status(table.retry(queued), StatusCode::bad_descriptor);
    const auto retired_ticket = queued;
    CHECK(table.schedule_one([&](const Table::Ticket &ticket) -> Status {
        queued = ticket; ++callbacks; return {};
    }));
    check_status(table.acknowledge_reclaimed(retired_ticket), StatusCode::bad_descriptor);
    CHECK(callbacks == 3 && table.acknowledge_reclaimed(queued));
    check_status(table.acknowledge_reclaimed(queued), StatusCode::bad_descriptor);
    CHECK(table.active_records() == 1);
    CHECK(table.observe({&mount, 8, 12, 4}));
    check_result(table.snapshot(key), StatusCode::not_found);
    CHECK(table.active_records() == 2);
}

void capacities()
{
    int mount{};
    Ext4InodeLifetimeTable<1, 2> table{};
    const Ext4InodeLifetimeKey key{&mount, 1, 14, 0};
    CHECK(table.observe(key));
    check_status(table.observe({&mount, 1, 15, 0}), StatusCode::no_memory);
    auto first = table.retain(key, Ext4InodeHold::descriptor);
    auto second = table.retain(key, Ext4InodeHold::pin);
    CHECK(first && second);
    check_result(table.clone(first.value(), Ext4InodeHold::io), StatusCode::no_memory);
    auto snapshot = table.snapshot(key);
    CHECK(snapshot && snapshot.value().holds == std::array<u32, 4>{1, 0, 0, 1});
    Ext4InodeLifetimeTable<1, 2> unrelated{};
    auto other = unrelated.retain(key, Ext4InodeHold::pin);
    check_result(other, StatusCode::not_found);
    check_status(unrelated.release(first.value()), StatusCode::bad_descriptor);
    CHECK(table.release(first.value()));
    CHECK(table.release(second.value()));
    CHECK(table.mark_orphan(key));
}

class TrackedNode final : public Node
{
  public:
    Table *table{};
    Ext4InodeLifetimeKey key{};
    std::array<std::optional<Table::Lease>, 8> leases{};
    usize retained{};
    Node *child{};
    bool directory{};
    std::string_view name() const override { return directory ? "/" : "file"; }
    Metadata metadata() const override
    { return {.type = directory ? NodeType::directory : NodeType::regular,
              .mode = mode_for(directory ? NodeType::directory : NodeType::regular, 0755)}; }
    Result<FileBuffer> read(usize, usize) const override { return FileBuffer{}; }
    Status write(usize, std::span<const std::byte>) override { return Status::unsupported(); }
    Status retain() override
    {
        if (directory) return {};
        if (retained == leases.size()) return Status::no_memory("test fd slots");
        auto lease = table->retain(key, Ext4InodeHold::descriptor);
        if (!lease) return lease.status();
        leases[retained++].emplace(std::move(lease).value());
        return {};
    }
    Status release() override
    {
        if (directory) return {};
        if (!retained) return Status::bad_descriptor();
        auto &held = leases[--retained];
        const auto status = table->release(*held);
        held.reset();
        return status;
    }
    Node *lookup(std::string_view segment) override
    { return directory && segment == "file" ? child : nullptr; }
    Status create(std::string_view, NodeType) override { return Status::unsupported(); }
    Status chmod(u32) override { return Status::unsupported(); }
    Status chown(u32, u32) override { return Status::unsupported(); }
};

void vfs_path_reference()
{
    int mount_id{};
    Table table{};
    const Ext4InodeLifetimeKey key{&mount_id, 11, 14, 0};
    CHECK(table.observe(key));
    TrackedNode root{}, file{};
    root.directory = true; root.child = &file;
    file.table = &table; file.key = key;
    VirtualFileSystem vfs;
    CHECK(vfs.mount("/", root));
    auto path = vfs.resolve({}, "/file");
    CHECK(path && path.value().node() == &file && file.retained == 1);
    CHECK(table.mark_orphan(key));
    auto io = table.clone(*file.leases[0], Ext4InodeHold::io);
    CHECK(io);
    CHECK(path.value().release());
    CHECK(file.retained == 0);
    auto snapshot = table.snapshot(key);
    CHECK(snapshot && snapshot.value().holds == std::array<u32, 4>{0, 0, 1, 0} &&
          snapshot.value().phase == Ext4InodeLifetimePhase::retired);
    CHECK(table.release(io.value()));
    Table::Ticket queued{};
    CHECK(table.schedule_one([&](const Table::Ticket &ticket) -> Status {
        CHECK(table.snapshot(key)); queued = ticket; return {};
    }));
    CHECK(table.acknowledge_reclaimed(queued));
}
} // namespace

int main()
{
    lifecycle(); capacities(); vfs_path_reference();
    std::printf("PASS inode lifetime: move-only counted leases, fd mapping io pin, VFS PathReference, retry, stale/capacity guards\n");
}
