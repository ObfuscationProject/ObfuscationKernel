// Complete-call interruption model over the production common-storage owner.
#define OK_CHECKED_OWNERSHIP_NO_MAIN
#include "ext4_checked_ownership_tests.cpp"

struct CommonMount
{
    Disk disk;
    Mount::Registry registry;
    std::unique_ptr<Mount::CommonStorage> storage = std::make_unique<Mount::CommonStorage>();
    Mount mount{*storage};
    Mount::Registry::Registration key{};
    explicit CommonMount(const Bytes &bytes) : disk(bytes)
    {
        auto registered = registry.register_device(disk, 17); CHECK(registered); key = registered.value();
        const auto status = mount.mount(registry, key, {0, bytes.size() / 512});
        if (!status) std::fprintf(stderr, "ownership common mount code=%u message=%.*s\n",
            unsigned(status.code()), int(status.message().size()), status.message().data());
        CHECK(status && mount.root());
    }
};

usize ownership_recovery_cuts{}, ownership_active_recoveries{};

void ownership_complete_call_cuts(const Bytes &clean, std::string_view name, bool mode = false)
{
    clock_model::set(sample()); auto successful = std::make_unique<CommonMount>(clean);
    auto target = successful->mount.root()->lookup_child_retained(name); CHECK(target);
    const auto initial = target.value()->query_metadata(); CHECK(initial);
    const auto number = static_cast<u32>(initial.value().attributes.inode);
    const auto inode = inode_offset(clean, number);
    const auto start = successful->disk.trace.size();
    const auto apply = [&](Node &node) {
        return mode ? node.change_mode(0755, user::root_credentials()) :
            node.change_owner(1007, 1008, user::root_credentials());
    };
    CHECK(apply(*target.value()).ok());
    std::vector<char> operations;
    for (usize index = start; index < successful->disk.trace.size(); ++index)
        if (successful->disk.trace[index].operation != 'r') operations.push_back(successful->disk.trace[index].operation);
    CHECK(!operations.empty() && target.value()->release());
    for (usize prefix = 0; prefix < operations.size(); ++prefix)
    {
        auto interrupted = std::make_unique<CommonMount>(clean);
        auto node = interrupted->mount.root()->lookup_child_retained(name); CHECK(node);
        usize writes{}, flushes{};
        for (usize index = 0; index < prefix; ++index)
            operations[index] == 'w' ? ++writes : ++flushes;
        if (operations[prefix] == 'w') interrupted->disk.fail_write = interrupted->disk.writes + writes + 1;
        else interrupted->disk.fail_flush = interrupted->disk.flushes + flushes + 1;
        CHECK(!apply(*node.value()).ok());
        CHECK(interrupted->mount.report().phase == Ext4MountPhase::failed);
        CHECK(node.value()->release()); interrupted->disk.crash();
        const bool active = be32(interrupted->disk.durable, journal(clean) + 28) != 0;
        auto resumed = std::make_unique<CommonMount>(interrupted->disk.durable);
        auto restored = resumed->mount.root()->lookup_child_retained(name); CHECK(restored);
        auto metadata = restored.value()->query_metadata(); CHECK(metadata);
        const auto old_version = r32(clean, inode + 36), version = r32(resumed->disk.durable, inode + 36);
        const bool committed = version == old_version + 1;
        CHECK(version == old_version || committed);
        CHECK(metadata.value().uid == (committed && !mode ? 1007 : initial.value().uid) &&
            metadata.value().gid == (committed && !mode ? 1008 : initial.value().gid));
        CHECK((metadata.value().mode & 07777) == (committed && mode ? 0755 :
            committed ? initial.value().mode & 01777 : initial.value().mode & 07777));
        const auto &expected = committed ? successful->disk.durable : clean;
        CHECK(std::equal(resumed->disk.durable.begin() + inode, resumed->disk.durable.begin() + inode + 256,
            expected.begin() + inode));
        // The retained constructor without a startup workspace must recover
        // the same checked metadata transaction and publish identical bytes.
        auto compatibility = std::make_unique<Mounted>(interrupted->disk.durable,
            Ext4MountedPolicy::ordered_data_dirty_noatime);
        CHECK(compatibility->disk.live == resumed->disk.live &&
            compatibility->disk.durable == resumed->disk.durable);
        CHECK(!be32(resumed->disk.durable, journal(clean) + 28));
        CHECK(r32(resumed->disk.durable, 1120) & 4);
        const auto report = resumed->mount.startup_report();
        CHECK(report.phase == Ext4StartupPhase::complete && report.strict_ordinary_admission);
        if (active)
        {
            CHECK(committed && report.initial_family == Ext4StartupFamily::legacy_active);
            ++ownership_active_recoveries;
        }
        CHECK(restored.value()->release()); ++ownership_recovery_cuts;
    }
}

int main(int argc, char **argv)
{
    CHECK(argc == 2); std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    const Bytes clean((std::istreambuf_iterator<char>(input)), {}); CHECK(!clean.empty());
    ownership_complete_call_cuts(clean, "seed"); ownership_complete_call_cuts(clean, "child");
    ownership_complete_call_cuts(clean, "seed", true); ownership_complete_call_cuts(clean, "child", true);
    CHECK(ownership_active_recoveries);
    std::printf("CHECKED_OWNERSHIP_RECOVERY checks=%zu cuts=%zu active=%zu owner_devices=1 owner_buffers=9 complete_calls=1 torn_media=0 guest=0 hardware=0\n",
        checks, ownership_recovery_cuts, ownership_active_recoveries);
}
