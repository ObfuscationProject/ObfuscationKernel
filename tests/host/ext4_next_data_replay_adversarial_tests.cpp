#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main next_data_reference_model_main
#include "ext4_next_data_replay_tests.cpp"
#undef main
#pragma GCC diagnostic pop

namespace
{
void final_inode_payload(Bytes &image, Published &published)
{
    published.plan.after[3] = get(image, published.plan.targets[3]);
    put(image, published.journal + 5, published.plan.after[3]);
    journal_payload_checksum(image, published, 3);
    for (usize index = 0; index < 4; ++index)
        home_after(image, published, index);
}

bool terminal(Bytes image, Ext4ReplayPolicy policy, const char *label)
{
    Disk disk(std::move(image));
    auto completion = std::make_unique<Ext4RecoveryCompletion<4, 2>>(false, true, policy);
    const auto result = completion->run(disk, {0, disk.image.size() / 512});
    const bool safe = !result && disk.writes == 0 && disk.flushes == 0;
    std::printf("ADVERSARIAL %s rejected=%u writes=%zu flushes=%zu code=%u reason=%.*s\n",
        label, !result, disk.writes, disk.flushes, u32(result.code()),
        int(result.message().size()), result.message().data());
    return safe;
}

bool accepted(Bytes image, Ext4ReplayPolicy policy, const char *label,
              usize expected_writes, usize expected_flushes)
{
    Disk disk(std::move(image));
    auto completion = std::make_unique<Ext4RecoveryCompletion<4, 2>>(false, true, policy);
    const auto result = completion->run(disk, {0, disk.image.size() / 512});
    const bool okay = result && completion->report().complete() &&
                      completion->report().validated_actions == 4 &&
                      disk.writes == expected_writes && disk.flushes == expected_flushes;
    std::printf("ADVERSARIAL %s accepted=%u writes=%zu flushes=%zu code=%u\n",
        label, bool(result), disk.writes, disk.flushes, u32(result.code()));
    return okay;
}
} // namespace

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    const Bytes source((std::istreambuf_iterator<char>(input)), {});
    CHECK(source.size() == 32 * 1024 * 1024);
    const auto published = publish(source);
    Published with_sibling{};
    usize failures = 0, cases = 0;
    const auto verify = [&](bool okay) { ++cases; if (!okay) ++failures; };

    verify(accepted(published.image, Ext4ReplayPolicy::bounded_data_allocation,
                    "bounded-union-next-old-home", 5, 4));
    verify(terminal(published.image, Ext4ReplayPolicy::first_data_allocation,
                    "first-only-policy-rejects-next"));
    {
        // inode 13 was created with one block before inode 14's append. Give
        // it the canonical post-first-write timestamp/version shape: when all
        // homes are after, inverting its already allocated block is also a
        // valid first-data predecessor, although the log belongs to inode 14.
        // Construct a real first-write plan for inode 13 from a plausible
        // SDK46 empty inode, then place that completed result before inode
        // 14's first write and append. This keeps the sibling unchanged by
        // the next-data transaction itself.
        Bytes source_with_sibling = source;
        const usize sibling = inode_offset(source_with_sibling, 13);
        const usize empty = inode_offset(source_with_sibling, 14);
        std::copy_n(source.begin() + empty, 256, source_with_sibling.begin() + sibling);
        inode_crc(source_with_sibling, 13, sibling);
        constexpr u32 sibling_block = 534;
        const usize bitmap = r32(source_with_sibling, descriptor(source_with_sibling, 0)) * 1024;
        const u32 local = sibling_block - 1;
        CHECK(source_with_sibling[bitmap + local / 8] & (1U << (local % 8)));
        source_with_sibling[bitmap + local / 8] &= u8(~(1U << (local % 8)));
        const usize group = descriptor(source_with_sibling, 0);
        w16(source_with_sibling, group + 12, r16(source_with_sibling, group + 12) + 1);
        w16(source_with_sibling, group + 24,
            u16(crc(seed(source_with_sibling), source_with_sibling, bitmap, 1024)));
        group_crc(source_with_sibling, 0, group);
        w32(source_with_sibling, 1024 + 12, r32(source_with_sibling, 1024 + 12) + 1);
        super_crc(source_with_sibling);
        Disk before_first(source_with_sibling);
        Ext4RecoveryBinding first_binding;
        CHECK(first_binding.inspect(before_first, {0, before_first.image.size() / 512}));
        const auto selected = Ext4FirstDataPlanner::select_block(*first_binding.info(),
            get(source_with_sibling, 1), get(source_with_sibling, 2),
            get(source_with_sibling, u32(bitmap / 1024)), 0);
        CHECK(selected && selected.value() == sibling_block);
        Ext4FirstDataPlan sibling_plan{};
        CHECK(Ext4FirstDataPlanner::plan(*first_binding.info(),
            {13, 1024, 1700000100, 10},
            {0, sibling_block, u32(sibling / 1024), get(source_with_sibling, 1),
             get(source_with_sibling, 2), get(source_with_sibling, u32(bitmap / 1024)),
             get(source_with_sibling, u32(sibling / 1024))}, sibling_plan));
        for (usize index = 0; index < 4; ++index)
            put(source_with_sibling, sibling_plan.targets[index], sibling_plan.after[index]);
        with_sibling = publish(source_with_sibling);
        Bytes image = with_sibling.image;
        for (usize index = 0; index < 4; ++index) home_after(image, with_sibling, index);
        verify(accepted(image, Ext4ReplayPolicy::first_data_allocation,
                        "sibling-first-only", 1, 2));
        verify(accepted(image, Ext4ReplayPolicy::next_data_allocation,
                        "target-next-only", 1, 2));
        verify(accepted(std::move(image), Ext4ReplayPolicy::bounded_data_allocation,
                        "all-after-prior-first-sibling", 1, 2));
    }
    {
        Bytes image = published.image;
        const usize at = inode_offset(image, 14);
        w32(image, at + 60, 595);
        inode_crc(image, 14, at);
        verify(terminal(std::move(image), Ext4ReplayPolicy::bounded_data_allocation,
                        "old-inode-prefix-changed"));
    }
    {
        auto changed = published;
        Bytes image = changed.image;
        home_after(image, changed, 3);
        const usize at = inode_offset(image, 14);
        w32(image, at + 16, r32(image, at + 16) + 1);
        inode_crc(image, 14, at);
        final_inode_payload(image, changed);
        verify(terminal(std::move(image), Ext4ReplayPolicy::bounded_data_allocation,
                        "all-after-unequal-mtime-ctime"));
    }
    {
        auto changed = published;
        Bytes image = changed.image;
        home_after(image, changed, 3);
        const usize at = inode_offset(image, 14);
        w32(image, at + 36, 0);
        w32(image, at + 152, 0);
        inode_crc(image, 14, at);
        final_inode_payload(image, changed);
        verify(terminal(std::move(image), Ext4ReplayPolicy::bounded_data_allocation,
                        "all-after-zero-version"));
    }
    {
        auto changed = published;
        Bytes image = changed.image;
        home_after(image, changed, 3);
        const usize target = inode_offset(image, 14), sibling = inode_offset(image, 16);
        CHECK(r16(image, target + 42) == 2 && r32(image, target + 60) == 535 &&
              r32(image, target + 72) == changed.plan.physical &&
              changed.plan.physical == 596);
        CHECK(r16(image, sibling + 42) == 2 && r32(image, sibling + 72) == 543 &&
              r16(image, sibling + 68) == 53);
        // Swap the old prefix with the sibling's final block. Both files still
        // have unique allocated mappings, but the appended extent at 596 now
        // follows 595 and must have been coalesced by the canonical writer.
        w32(image, target + 60, 595);
        w16(image, sibling + 68, 52);
        w16(image, sibling + 42, 3);
        w32(image, sibling + 76, 60);
        w16(image, sibling + 80, 1);
        w32(image, sibling + 84, 535);
        inode_crc(image, 14, target);
        inode_crc(image, 16, sibling);
        final_inode_payload(image, changed);
        verify(terminal(std::move(image), Ext4ReplayPolicy::bounded_data_allocation,
                        "all-after-noncanonical-contiguous-append"));
    }
    {
        Bytes image = published.image;
        for (usize index = 0; index < 4; ++index) home_after(image, published, index);
        const usize at = inode_offset(image, 12);
        w32(image, at + 60, published.plan.physical);
        inode_crc(image, 12, at);
        verify(terminal(std::move(image), Ext4ReplayPolicy::bounded_data_allocation,
                        "all-after-candidate-alias"));
    }
    {
        auto changed = published;
        Bytes image = changed.image;
        home_after(image, changed, 3);
        const usize sibling = inode_offset(image, 13);
        // inode 16 already owns physical block 543. The candidate and old
        // prefix remain unique, so this probes completeness of final-state
        // ownership validation for an unrelated inode pair.
        CHECK(r32(image, sibling + 60) == 534);
        w32(image, sibling + 60, 543);
        inode_crc(image, 13, sibling);
        final_inode_payload(image, changed);
        verify(terminal(std::move(image), Ext4ReplayPolicy::bounded_data_allocation,
                        "all-after-unrelated-owner-alias"));
    }
    {
        auto changed = with_sibling;
        Bytes image = changed.image;
        home_after(image, changed, 3);
        const usize other = inode_offset(image, 16);
        CHECK(r16(image, other + 68) == 53 && r32(image, other + 72) == 543 &&
              changed.plan.physical == 596);
        // A sibling in the journaled table claims the true next candidate;
        // the older first-write sibling still supplies a valid independent
        // all-after proof if the union accepts only one surviving match.
        w16(image, other + 68, 54);
        w32(image, other + 4, 62465);
        w32(image, other + 28, 122);
        inode_crc(image, 16, other);
        final_inode_payload(image, changed);
        verify(terminal(std::move(image), Ext4ReplayPolicy::bounded_data_allocation,
                        "all-after-sibling-masks-target-alias"));
    }
    std::printf("ADVERSARIAL next-data replay cases=%zu failures=%zu\n", cases, failures);
    return failures ? 1 : 0;
}
