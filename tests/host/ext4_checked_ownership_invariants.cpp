// Hosted actual-provider attacks; only this target disables private access.
#define OK_CHECKED_OWNERSHIP_NO_MAIN
#include "ext4_checked_ownership_tests.cpp"

using OwnedNode = Mount::MountedNode;
usize ownership_identity_controls{}, ownership_original_controls{};
usize ownership_replay_refusals{};

void ownership_identity(const Bytes &clean)
{
    for (usize variant = 0; variant < 8; ++variant)
    {
        auto m = std::make_unique<Mounted>(clean, Ext4MountedPolicy::ordered_data_dirty_noatime);
        auto found = m->mount.root()->lookup_child_retained("child"); CHECK(found);
        auto *node = static_cast<OwnedNode *>(found.value()); const OwnedNode original = *node;
        OwnedNode copied = original; auto &duplicate = m->mount.nodes_[20]; CHECK(!duplicate.inode_);
        if (variant == 1) node->inode_ = 0;
        if (variant == 2) ++node->lifetime_key_.generation;
        if (variant == 3) ++node->lifetime_key_.session;
        if (variant == 4) ++node->lifetime_key_.inode;
        if (variant == 5) node->mount_ = nullptr;
        if (variant == 6) duplicate = *node;
        const auto reads = m->disk.reads, writes = m->disk.writes, flushes = m->disk.flushes;
        const auto result = variant == 0 ? copied.change_owner(1, 2, user::root_credentials()) :
            m->mount.change_owner(*node, 1, 2, user::root_credentials());
        if (variant != 7)
        {
            CHECK(!result.ok() && result.status().code() == StatusCode::bad_descriptor);
            CHECK(m->disk.reads == reads && m->disk.writes == writes && m->disk.flushes == flushes);
            CHECK(m->mount.report().phase == Ext4MountPhase::readable);
        }
        else CHECK(result.ok());
        *node = original; if (variant == 6) duplicate = {};
        auto lifetime = m->mount.lifetime_.snapshot(original.lifetime_key_); CHECK(lifetime);
        CHECK(!lifetime.value().holds[static_cast<usize>(Ext4InodeHold::io)]);
        CHECK(!m->mount.mutation_context_ && !m->mount.mutation_lease_ && !m->mount.mutation_node_);
        CHECK(node->release()); ++ownership_identity_controls;
    }
    for (bool valid_checksum : {false, true})
    {
        auto m = std::make_unique<Mounted>(clean, Ext4MountedPolicy::ordered_data_dirty_noatime);
        auto found = m->mount.root()->lookup_child_retained("child"); CHECK(found);
        auto *node = static_cast<OwnedNode *>(found.value());
        const auto at = inode_offset(m->disk.live, node->inode_);
        w32(m->disk.live, at + 100, node->lifetime_key_.generation + 1);
        if (valid_checksum) inode_crc(m->disk.live, node->inode_, at);
        const auto writes = m->disk.writes, flushes = m->disk.flushes;
        CHECK(!node->change_owner(1, 2, user::root_credentials()).ok());
        CHECK(m->mount.report().phase == Ext4MountPhase::failed && m->disk.writes == writes && m->disk.flushes == flushes);
        CHECK(node->release()); ++ownership_identity_controls;
    }
}

void ownership_original_authority(const Bytes &clean)
{
    for (usize variant = 0; variant < 4; ++variant)
    {
        auto m = std::make_unique<Mounted>(clean, Ext4MountedPolicy::ordered_data_dirty_noatime);
        auto found = m->mount.root()->lookup_child_retained("child"); CHECK(found);
        auto *node = static_cast<OwnedNode *>(found.value()); const auto key = node->lifetime_key_;
        bool called{}; const auto writes = m->disk.writes, flushes = m->disk.flushes;
        m->disk.hook = [&](char) {
            if (called) return;
            called = true;
            CHECK(m->mount.mutation_context_ && m->mount.mutation_context_->key == key);
            CHECK(node->change_owner(1, 2, user::root_credentials()).status().code() == StatusCode::busy);
            if (variant == 0) m->mount.mutation_context_ = nullptr;
            if (variant == 1) m->mount.mutation_node_ = nullptr;
            if (variant == 2) m->mount.mutation_lease_ = nullptr;
            if (variant == 3) ++m->mount.mutation_key_.generation;
        };
        CHECK(!node->change_owner(1, 2, user::root_credentials()).ok()); m->disk.hook = {};
        CHECK(called && m->mount.report().phase == Ext4MountPhase::failed);
        CHECK(m->disk.writes == writes && m->disk.flushes == flushes);
        auto lifetime = m->mount.lifetime_.snapshot(key); CHECK(lifetime);
        CHECK(!lifetime.value().holds[static_cast<usize>(Ext4InodeHold::io)]);
        CHECK(!m->mount.mutation_context_ && !m->mount.mutation_lease_ && !m->mount.mutation_node_);
        CHECK(node->release()); ++ownership_original_controls;
    }
}

void ownership_replay_policy(const Bytes &clean)
{
    auto mounted = std::make_unique<Mounted>(clean);
    auto child = mounted->mount.root()->lookup_child("child"); CHECK(child);
    auto metadata = child.value()->query_metadata(); CHECK(metadata);
    const auto number = static_cast<u32>(metadata.value().attributes.inode);
    const auto permissions = static_cast<u16>(metadata.value().mode & 01777);
    for (usize variant = 0; variant < 10; ++variant)
    {
        Ext4InodeMetadataUpdate update{number, 1007, 1008, 1700000077, 123456789, permissions};
        if (variant == 0) update.permissions = metadata.value().mode & 07777;
        if (variant == 1) update.permissions = permissions ^ 0002;
        auto image = mounted_publication(clean, update);
        const auto at = journal(image) + 3072 + inode_offset(image, number) % 1024;
        if (variant == 2) w32(image, at + 4, r32(image, at + 4) + 1024);
        if (variant == 3) w32(image, at + 16, r32(image, at + 16) + 1);
        if (variant == 4) w32(image, at + 100, r32(image, at + 100) + 1);
        if (variant == 5) w16(image, at + 26, r16(image, at + 26) + 1);
        if (variant == 6) w32(image, at + 36, r32(image, at + 36) + 1);
        if (variant == 7) w32(image, at + 12, 0x80000000u);
        if (variant == 8) w32(image, at + 132, (123456789u << 2) | 1u);
        if (variant == 9) w32(image, at + 40, r32(image, at + 40) + 1);
        inode_crc(image, number, at); linux_payload_crc(image, true);
        Disk transport(image); Ext4ReplayPreview<> preview(false, false, Ext4ReplayPolicy::checked_ownership);
        CHECK(!preview.open(transport, {0, image.size() / 512}));
        CHECK(!transport.writes && !transport.flushes && transport.live == image && transport.durable == image);
        ++ownership_replay_refusals;
    }
    // The unchanged public structural policy still rejects directory owners.
    const Ext4InodeMetadataUpdate update{number, 1007, 1008, 1700000077, 123456789, permissions};
    const auto image = mounted_publication(clean, update);
    Disk transport(image); Ext4ReplayPreview<> legacy;
    CHECK(!legacy.open(transport, {0, image.size() / 512}) && !transport.writes && !transport.flushes);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2); std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    const Bytes clean((std::istreambuf_iterator<char>(input)), {}); CHECK(!clean.empty());
    clock_model::set(sample()); ownership_identity(clean); ownership_original_authority(clean); ownership_replay_policy(clean);
    std::printf("CHECKED_OWNERSHIP_INVARIANTS checks=%zu identities=%zu originals=%zu replay_refusals=%zu hosted=1 guest=0 hardware=0\n",
        checks, ownership_identity_controls, ownership_original_controls, ownership_replay_refusals);
}
