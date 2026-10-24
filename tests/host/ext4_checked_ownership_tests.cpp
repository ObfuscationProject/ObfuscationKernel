// Hosted actual POSIX/VFS/mounted provider; no user instructions or hardware.
#define main retained_mode_tests_main
#include "ext4_journalled_mode_tests.cpp"
#undef main

constexpr u32 keep_owner = ~u32{0};
usize ownership_operations{}, ownership_faults{};

void ownership_fields(Mounted &m, const Bytes &before, u32 inode, u32 uid, u32 gid, u16 permissions)
{
    const auto at = inode_offset(before, inode);
    CHECK(r16(m.disk.live, at + 2) == u16(uid) && r16(m.disk.live, at + 120) == u16(uid >> 16));
    CHECK(r16(m.disk.live, at + 24) == u16(gid) && r16(m.disk.live, at + 122) == u16(gid >> 16));
    CHECK(r16(m.disk.live, at) == ((r16(before, at) & 0170000) | permissions));
    CHECK(r32(m.disk.live, at + 12) == 1700000077 && r32(m.disk.live, at + 132) == (123456789u << 2));
    const u64 old_version = u64{r32(before, at + 36)} | (u64{r32(before, at + 152)} << 32);
    const u64 version = u64{r32(m.disk.live, at + 36)} | (u64{r32(m.disk.live, at + 152)} << 32);
    CHECK(version == old_version + 1);
    for (usize byte = 0; byte < 256; ++byte)
    {
        const bool mutable_byte = byte < 4 || (byte >= 12 && byte < 16) || (byte >= 24 && byte < 26) ||
            (byte >= 36 && byte < 40) || (byte >= 120 && byte < 126) || (byte >= 130 && byte < 136) ||
            (byte >= 152 && byte < 156);
        if (!mutable_byte) CHECK(m.disk.live[at + byte] == before[at + byte]);
    }
    if (m.mount.policy() != Ext4MountedPolicy::ordered_data_dirty_noatime)
    {
        const Ext4InodeMetadataUpdate update{inode, uid, gid, 1700000077, 123456789, permissions};
        CHECK(m.disk.live == mounted_completion(mounted_publication(before, update), inode));
    }
    CHECK(m.disk.live == m.disk.durable && m.mount.report().phase == Ext4MountPhase::readable);
    CHECK(!m.mount.buffers().read_pins && !m.mount.buffers().in_flight);
    ++ownership_operations;
}

void ownership_composition(const Bytes &clean, Ext4MountedPolicy policy)
{
    clock_model::set(sample()); auto m = std::make_unique<Mounted>(clean, policy);
    auto services = std::make_unique<Services>(*m->mount.root()); auto &p = services->service;
    CHECK(p.set_credentials(user::root_credentials()));
    auto fd = p.open("/journal/seed", posix::o_RDONLY); CHECK(fd);
    auto duplicate = p.duplicate(fd.value()); CHECK(duplicate);
    const auto initial = p.fstat(fd.value()); CHECK(initial);
    auto before = m->disk.live;
    CHECK(p.change_owner_fd(fd.value(), keep_owner, keep_owner).ok());
    ownership_fields(*m, before, 12, initial.value().uid, initial.value().gid, initial.value().mode & 07777);
    before = m->disk.live;
    CHECK(p.change_owner_at(-999, "/alias/seed", 0x12345678, keep_owner).ok());
    ownership_fields(*m, before, 12, 0x12345678, initial.value().gid, initial.value().mode & 01777);
    before = m->disk.live;
    CHECK(p.change_owner_at(duplicate.value(), "", keep_owner, 0x89abcdef, posix::at_EMPTY_PATH).ok());
    ownership_fields(*m, before, 12, 0x12345678, 0x89abcdef, initial.value().mode & 01777);
    auto changed = p.fstat(duplicate.value()); CHECK(changed);
    CHECK(changed.value().uid == 0x12345678 && changed.value().gid == 0x89abcdef);
    CHECK(changed.value().attributes.inode == initial.value().attributes.inode &&
        changed.value().size == initial.value().size && changed.value().blocks == initial.value().blocks &&
        changed.value().link_count == initial.value().link_count &&
        changed.value().attributes.access.seconds == initial.value().attributes.access.seconds &&
        changed.value().attributes.access.nanoseconds == initial.value().attributes.access.nanoseconds &&
        changed.value().attributes.modification.seconds == initial.value().attributes.modification.seconds &&
        changed.value().attributes.modification.nanoseconds == initial.value().attributes.modification.nanoseconds);
    auto child = p.open("/journal/child", posix::o_RDONLY | posix::o_DIRECTORY); CHECK(child);
    auto directory_before = p.fstat(child.value()); CHECK(directory_before);
    const auto number = static_cast<u32>(directory_before.value().attributes.inode);
    before = m->disk.live;
    CHECK(p.change_mode_at(child.value(), "", 0755, posix::at_EMPTY_PATH).ok());
    ownership_fields(*m, before, number, directory_before.value().uid, directory_before.value().gid, 0755);
    before = m->disk.live;
    CHECK(p.change_mode_fd(child.value(), directory_before.value().mode & 07777).ok());
    ownership_fields(*m, before, number, directory_before.value().uid, directory_before.value().gid,
        directory_before.value().mode & 07777);
    before = m->disk.live;
    CHECK(p.change_owner_at(posix::at_FDCWD, "/alias/child-link", 1007, 1008).ok());
    ownership_fields(*m, before, number, 1007, 1008, directory_before.value().mode & 01777);
    CHECK(p.set_identity(1007, 1008));
    CHECK(p.open("/journal/child/leaf", posix::o_RDONLY));
    const auto writes = m->disk.writes, flushes = m->disk.flushes, clocks = clock_model::calls(); before = m->disk.live;
    CHECK(p.change_owner_fd(child.value(), 1007, 1008).status().code() == StatusCode::permission);
    CHECK(p.change_owner_at(child.value(), "", keep_owner, keep_owner, posix::at_EMPTY_PATH).status().code() == StatusCode::permission);
    CHECK(clock_model::calls() == clocks); unchanged(*m, before, writes, flushes);
    CHECK(p.set_identity(1017, 1018));
    CHECK(p.open("/journal/child/leaf", posix::o_RDONLY).status().code() == StatusCode::denied);
    CHECK(p.set_credentials(user::root_credentials()));
    CHECK(p.chdir("/journal/child")); before = m->disk.live;
    CHECK(p.change_owner_at(posix::at_FDCWD, "", keep_owner, 1009, posix::at_EMPTY_PATH).ok());
    ownership_fields(*m, before, number, 1007, 1009, directory_before.value().mode & 01777);
    CHECK(p.chdir("/"));
    auto parent = p.stat("/journal"); CHECK(parent && parent.value().uid == 0 && parent.value().gid == 0);
    auto dir = p.open("/journal", posix::o_RDONLY | posix::o_DIRECTORY); CHECK(dir); before = m->disk.live;
    CHECK(p.change_owner_at(dir.value(), "seed", 1000, 1001).ok());
    ownership_fields(*m, before, 12, 1000, 1001, initial.value().mode & 01777);
    auto fork = p.fork(); CHECK(fork && p.activate(fork.value()));
    CHECK(p.fstat(fd.value()).value().uid == 1000);
    auto &child_resources = services->scheduler.find(fork.value())->resources();
    for (i32 descriptor = 3; descriptor < static_cast<i32>(posix::max_open_files); ++descriptor)
        if (child_resources.files.contains(descriptor)) CHECK(p.close(descriptor));
    if (child_resources.cwd_node) { CHECK(child_resources.cwd_node->release()); child_resources.cwd_node = nullptr; }
    CHECK(p.activate(services->pid));
    const auto stable = m->disk.live; const auto wc = m->disk.writes, fc = m->disk.flushes;
    CHECK(p.change_owner_fd(-999, 1, 2).status().code() == StatusCode::bad_descriptor);
    CHECK(p.change_owner_fd(0, 1, 2).kind() == Change::Kind::unavailable);
    CHECK(p.change_owner_at(posix::at_FDCWD, "", 1, 2).status().code() == StatusCode::not_found);
    CHECK(p.change_owner_at(posix::at_FDCWD, "/journal/seed", 1, 2, 0x8000).status().code() == StatusCode::invalid_argument);
    CHECK(p.change_owner_at(fd.value(), "seed", 1, 2).status().code() == StatusCode::not_directory);
    CHECK(p.change_owner_at(posix::at_FDCWD, "/journal/seed-link", 1, 2, posix::at_SYMLINK_NOFOLLOW).kind() == Change::Kind::unavailable);
    CHECK(p.change_owner_at(posix::at_FDCWD, "/journal", 1, 2).kind() == Change::Kind::unavailable);
    auto seed = m->mount.root()->lookup_child("seed"); CHECK(seed && !seed.value()->chown(1, 2));
    unchanged(*m, stable, wc, fc);
}

void ownership_refusals(const Bytes &clean)
{
    auto legacy = std::make_unique<Mounted>(clean, Ext4MountedPolicy::maintenance_only);
    auto seed = legacy->mount.root()->lookup_child("seed"); CHECK(seed);
    const auto bytes = legacy->disk.live; const auto reads = legacy->disk.reads;
    CHECK(seed.value()->change_owner(1, 2, user::root_credentials()).kind() == Change::Kind::unavailable);
    CHECK(legacy->disk.live == bytes && legacy->disk.reads == reads && !legacy->disk.writes && !legacy->disk.flushes);
    auto m = std::make_unique<Mounted>(clean); auto s = std::make_unique<Services>(*m->mount.root()); auto &p = s->service;
    CHECK(p.set_credentials(user::root_credentials()));
    auto fd = p.open("/journal/seed", posix::o_RDONLY); CHECK(fd);
    const auto stable = m->disk.live; const auto wc = m->disk.writes, fc = m->disk.flushes;
    clock_model::fail(); CHECK(p.change_owner_fd(fd.value(), 1, 2).status().code() == StatusCode::io_error);
    unchanged(*m, stable, wc, fc);
    for (i64 seconds : {-1LL, 2147483648LL})
    {
        clock_model::set(sample(seconds)); CHECK(p.change_owner_fd(fd.value(), 1, 2).status().code() == StatusCode::overflow);
        unchanged(*m, stable, wc, fc);
    }
    for (usize invalid = 0; invalid < 4; ++invalid)
    {
        auto value = sample();
        if (invalid == 0) value.value.nanoseconds = 1000000000;
        if (invalid == 1) value.wall_generation = 0;
        if (invalid == 2) value.monotonic.generation = 0;
        if (invalid == 3) value.origin = time::RealtimeOrigin::none;
        clock_model::set(value); CHECK(p.change_owner_fd(fd.value(), 1, 2).status().code() == StatusCode::io_error);
        unchanged(*m, stable, wc, fc);
    }
}

void ownership_failures(const Bytes &clean, Ext4MountedPolicy policy)
{
    clock_model::set(sample()); usize reads{}, writes{}, flushes{};
    {
        auto m = std::make_unique<Mounted>(clean, policy); auto node = m->mount.root()->lookup_child_retained("child"); CHECK(node);
        const auto rc = m->disk.reads, wc = m->disk.writes, fc = m->disk.flushes;
        CHECK(node.value()->change_owner(1007, 1008, user::root_credentials()).ok());
        reads = m->disk.reads - rc; writes = m->disk.writes - wc; flushes = m->disk.flushes - fc;
        CHECK(reads && writes && flushes && node.value()->release());
    }
    for (char kind : {'r', 'w', 'f'})
        for (usize point = 1; point <= (kind == 'r' ? reads : kind == 'w' ? writes : flushes); ++point)
        {
            auto m = std::make_unique<Mounted>(clean, policy); auto *root = m->mount.root();
            auto node = root->lookup_child_retained("child"); CHECK(node);
            if (kind == 'r') m->disk.fail_read = m->disk.reads + point;
            if (kind == 'w') m->disk.fail_write = m->disk.writes + point;
            if (kind == 'f') m->disk.fail_flush = m->disk.flushes + point;
            CHECK(!node.value()->change_owner(1007, 1008, user::root_credentials()).ok());
            CHECK(m->mount.report().phase == Ext4MountPhase::failed);
            const auto calls = m->disk.reads + m->disk.writes + m->disk.flushes;
            CHECK(!root->query_metadata() && !node.value()->change_owner(1, 2, user::root_credentials()).ok());
            CHECK(m->disk.reads + m->disk.writes + m->disk.flushes == calls && node.value()->release());
            ++ownership_faults;
        }
}

#ifndef OK_CHECKED_OWNERSHIP_NO_MAIN
int main(int argc, char **argv)
{
    CHECK(argc == 2); std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    const Bytes clean((std::istreambuf_iterator<char>(input)), {}); CHECK(!clean.empty());
    for (auto policy : {Ext4MountedPolicy::maintained_noatime, Ext4MountedPolicy::ordered_data_noatime,
                        Ext4MountedPolicy::ordered_data_dirty_noatime})
    { ownership_composition(clean, policy); ownership_failures(clean, policy); }
    ownership_refusals(clean);
    std::printf("CHECKED_OWNERSHIP_MODEL checks=%zu operations=%zu faults=%zu owner_devices=1 owner_buffers=9 guest=0 hardware=0\n",
        checks, ownership_operations, ownership_faults);
}
#endif
