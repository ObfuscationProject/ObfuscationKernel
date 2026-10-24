// Hosted provider and service model; actual guest execution is a separate gate.
#include "ok/posix/posix.hpp"
#include "ok/posix/native_stat.hpp"
#include "../kernel/points/file_attribute_checks.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
using namespace ok;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); std::abort(); } } while (false)
void absent_times(const fs::FileAttributes &value)
{
    CHECK(!value.has(fs::Attribute::access_time) && !value.has(fs::Attribute::modification_time) && !value.has(fs::Attribute::change_time));
    CHECK(value.access.source == fs::TimestampSource::unavailable && value.modification.source == fs::TimestampSource::unavailable && value.change.source == fs::TimestampSource::unavailable);
}
struct Fixture
{
    sched::Scheduler scheduler;
    fs::VirtualFileSystem vfs;
    driver::ConsoleDriver console;
    posix::PosixService service;
    sched::ProcessId parent;
    Fixture()
    {
        CHECK(console.start() && service.initialize(vfs, console, scheduler));
        auto process = scheduler.create_user_process("attributes", arch::configured_arch_operations().make_user_context({0x400000, 0x800000, 0}));
        CHECK(process); parent = process.value(); CHECK(service.activate(parent));
    }
    fs::FileAttributes stat(std::string_view path)
    {
        auto value = service.statat(posix::at_FDCWD, path); CHECK(value);
        auto attributes = value.value().attributes;
        CHECK(attributes.has(fs::Attribute::inode) && attributes.inode != 0 && attributes.has(fs::Attribute::device) && attributes.device.minor != 0);
        absent_times(attributes); return attributes;
    }
    void same(posix::Fd fd, const fs::FileAttributes &expected)
    {
        auto status = service.fstat(fd); CHECK(status);
        CHECK(status.value().attributes.inode == expected.inode && status.value().attributes.device == expected.device);
        absent_times(status.value().attributes);
    }
};
void live_identity()
{
    auto f = std::make_unique<Fixture>();
    auto descriptor = f->service.open("/tmp/original", posix::o_CREAT | posix::o_RDWR); CHECK(descriptor);
    auto original = f->stat("/tmp/original");
    CHECK(f->service.link("/tmp/original", "/tmp/alias"));
    CHECK(f->stat("/tmp/alias").inode == original.inode);
    CHECK(f->service.rename("/tmp/original", "/tmp/renamed"));
    CHECK(f->stat("/tmp/renamed").inode == original.inode);
    auto duplicate = f->service.duplicate(descriptor.value()); CHECK(duplicate);
    auto child = f->service.fork(); CHECK(child);
    CHECK(f->service.activate(child.value())); f->same(descriptor.value(), original); f->same(duplicate.value(), original);
    CHECK(f->service.close(descriptor.value()) && f->service.close(duplicate.value()));
    CHECK(f->service.activate(f->parent));
    CHECK(f->service.unlink("/tmp/alias") && f->service.unlink("/tmp/renamed"));
    f->same(descriptor.value(), original); f->same(duplicate.value(), original);
    auto newfd = f->service.open("/tmp/replacement", posix::o_CREAT | posix::o_RDWR); CHECK(newfd);
    CHECK(f->stat("/tmp/replacement").inode != original.inode);
    CHECK(f->service.close(descriptor.value()) && f->service.close(duplicate.value()) && f->service.close(newfd.value()));
    CHECK(f->service.unlink("/tmp/replacement"));
    for (usize i = 0; i < 200; ++i)
    {
        auto opened = f->service.open("/tmp/reused", posix::o_CREAT | posix::o_RDWR); CHECK(opened);
        auto current = f->stat("/tmp/reused"); CHECK(current.inode > original.inode); original = current;
        CHECK(f->service.close(opened.value()) && f->service.unlink("/tmp/reused"));
    }
    static constexpr std::array<std::byte, 3> bytes{std::byte{'o'}, std::byte{'k'}, std::byte{'!'}};
    CHECK(f->vfs.create_readonly_file("/tmp/archive", bytes));
    auto archive = f->stat("/tmp/archive");
    CHECK(f->vfs.create_readonly_file("/tmp/archive", bytes, 0555, true));
    CHECK(f->stat("/tmp/archive").inode == archive.inode);
    auto mutablefd = f->service.open("/tmp/mutable", posix::o_CREAT | posix::o_RDWR); CHECK(mutablefd);
    auto *node = f->vfs.lookup("/tmp/mutable"); CHECK(node);
    CHECK(node->write(0, bytes)); f->stat("/tmp/mutable");
    CHECK(node->truncate(1)); f->stat("/tmp/mutable");
    CHECK(node->chmod(0600)); f->stat("/tmp/mutable");
    CHECK(node->chown(0, 0)); f->stat("/tmp/mutable");
    CHECK(f->service.close(mutablefd.value()));
    auto console = f->service.fstat(1); CHECK(console && console.value().attributes.valid == 0);
    auto pipe = f->service.pipe(); CHECK(pipe);
    for (auto fd : pipe.value()) { auto value = f->service.fstat(fd); CHECK(value && value.value().attributes.valid == 0); CHECK(f->service.close(fd)); }
}
void provider_aliases()
{
    auto owner = std::make_unique<fs::VirtualFileSystem>();
    auto other = std::make_unique<fs::VirtualFileSystem>();
    auto namespace_ = std::make_unique<fs::VirtualFileSystem>();
    for (auto path : {"/one", "/alias", "/other"}) CHECK(namespace_->create(path, fs::NodeType::directory));
    CHECK(owner->create("/file", fs::NodeType::regular) && other->create("/file", fs::NodeType::regular));
    CHECK(namespace_->mount("/one", *owner->lookup("/")) && namespace_->mount("/alias", *owner->lookup("/")) && namespace_->mount("/other", *other->lookup("/")));
    auto first = namespace_->stat("/one/file"), alias = namespace_->stat("/alias/file"), distinct = namespace_->stat("/other/file");
    CHECK(first && alias && distinct);
    CHECK(first.value().attributes.inode == alias.value().attributes.inode && first.value().attributes.device == alias.value().attributes.device);
    CHECK(first.value().attributes.device != distinct.value().attributes.device);
    auto unknown = namespace_->query_metadata(*namespace_->lookup("/")); CHECK(unknown);
    fs::RamNode standalone; CHECK(standalone.configure("unknown", fs::NodeType::directory, nullptr));
    auto incomplete = namespace_->query_metadata(standalone); CHECK(incomplete && incomplete.value().attributes.valid == 0);
    CHECK(!namespace_->mount("/one", standalone));
    CHECK(namespace_->stat("/one/file").value().attributes.device == first.value().attributes.device);
}
void v2_wire_projection()
{
    static_assert(offsetof(ok_stat_v2, basic) == 0);
    static_assert(offsetof(ok_stat_v2, inode) >= sizeof(ok_stat));
    static_assert(OK_SYS_FSTAT_V2 == 1026 && OK_SYS_NEWFSTATAT_V2 == 1027);
    posix::FileStatus status{.type = fs::NodeType::regular, .size = 17, .mode = 0100640,
                             .uid = 1000, .gid = 1001, .link_count = 2,
                             .block_size = 4096, .blocks = 8};
    status.attributes.inode = 0xfedcba9876543210ULL;
    status.attributes.mark(fs::Attribute::device);
    struct Boundary { ok_stat_v2 value; u64 sentinel; } output{};
    output.sentinel = 0x123456789abcdef0ULL;
    auto *bytes = reinterpret_cast<std::byte *>(&output.value);
    auto compare_prefix_and_padding = [&] {
        const auto legacy = posix::native_file_status(status);
        for (usize i = 0; i < legacy.size(); ++i) CHECK(bytes[i] == legacy[i]);
        std::array<bool, sizeof(ok_stat_v2)> fields{};
        auto field = [&](usize offset, usize size) {
            for (usize i = 0; i < size; ++i) fields[offset + i] = true;
        };
        field(offsetof(ok_stat, type), sizeof(output.value.basic.type));
        field(offsetof(ok_stat, size), sizeof(output.value.basic.size));
        field(offsetof(ok_stat, mode), sizeof(output.value.basic.mode));
        field(offsetof(ok_stat, uid), sizeof(output.value.basic.uid));
        field(offsetof(ok_stat, gid), sizeof(output.value.basic.gid));
        field(offsetof(ok_stat, link_count), sizeof(output.value.basic.link_count));
        field(offsetof(ok_stat, block_size), sizeof(output.value.basic.block_size));
        field(offsetof(ok_stat, blocks), sizeof(output.value.basic.blocks));
        field(offsetof(ok_stat_v2, inode), sizeof(output.value.inode));
        for (usize i = 0; i < fields.size(); ++i)
            if (!fields[i]) CHECK(bytes[i] == std::byte{0});
        CHECK(output.sentinel == 0x123456789abcdef0ULL);
    };
    for (usize i = 0; i < sizeof(ok_stat_v2); ++i) bytes[i] = std::byte{0xa5};
    posix::write_native_file_status_v2(&output.value, status);
    compare_prefix_and_padding();
    CHECK(output.value.inode == 0); // A stale value without its valid bit is not an identity.
    status.attributes.mark(fs::Attribute::inode);
    for (usize i = 0; i < sizeof(ok_stat_v2); ++i) bytes[i] = std::byte{0xa5};
    posix::write_native_file_status_v2(&output.value, status);
    compare_prefix_and_padding();
    CHECK(output.value.inode == 0xfedcba9876543210ULL);
}
int main()
{
    CHECK(tests::check_file_attributes()); live_identity(); provider_aliases(); v2_wire_projection();
    struct Boundary { ok_stat value; u64 sentinel; } output{};
    output.sentinel = 0x123456789abcdef0ULL;
    posix::FileStatus internal{.type = fs::NodeType::symlink, .size = 42, .mode = 0120777, .uid = 1000, .gid = 1001, .link_count = 3, .block_size = 4096, .blocks = 16};
    internal.attributes.inode = 999; internal.attributes.valid = ~u32{0};
    auto *raw = reinterpret_cast<std::byte *>(&output.value);
    for (usize i = 0; i < sizeof(ok_stat); ++i) raw[i] = std::byte{0xa5};
    posix::write_native_file_status(&output.value, internal);
    std::array<bool, sizeof(ok_stat)> fields{};
    auto field = [&](usize offset, usize size) { for (usize i = 0; i < size; ++i) fields[offset + i] = true; };
    field(offsetof(ok_stat, type), sizeof(output.value.type));
    field(offsetof(ok_stat, size), sizeof(output.value.size));
    field(offsetof(ok_stat, mode), sizeof(output.value.mode));
    field(offsetof(ok_stat, uid), sizeof(output.value.uid));
    field(offsetof(ok_stat, gid), sizeof(output.value.gid));
    field(offsetof(ok_stat, link_count), sizeof(output.value.link_count));
    field(offsetof(ok_stat, block_size), sizeof(output.value.block_size));
    field(offsetof(ok_stat, blocks), sizeof(output.value.blocks));
    for (usize i = 0; i < sizeof(ok_stat); ++i) if (!fields[i]) CHECK(raw[i] == std::byte{0});
    CHECK(output.sentinel == 0x123456789abcdef0ULL && output.value.type == static_cast<u8>(fs::NodeType::symlink) && output.value.size == 42 && output.value.mode == 0120777 && output.value.uid == 1000 && output.value.gid == 1001 && output.value.link_count == 3 && output.value.block_size == 4096 && output.value.blocks == 16);
    std::puts("PASS file identity, timestamp provenance and native stat v2 wire model");
}
