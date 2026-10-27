#include "ext4_zero_truncate_preview_test_support.hpp"
#include <ok/block/sessions.hpp>
// The injected anchor drift reaches private state; build with -fno-access-control.
#include <ok/fs/ext4/alloc.hpp>

// Reuse the Session fixture and independently inject one minority anchor drift.
#define main inherited_author_session_models
#include "ext4_zero_truncate_session_preview_tests.cpp"
#undef main

namespace
{
struct AnchorMutation
{
    Preview &value;
    Session &foreign;
    usize kind{};
    static void apply(void *raw)
    {
        auto &m = *static_cast<AnchorMutation *>(raw);
        if (m.kind == 0) m.value.anchor_.first_ = &m.foreign;
        if (m.kind == 1) m.value.anchor_.second_ = &m.foreign;
        if (m.kind == 2) m.value.anchor_.third_ = &m.foreign;
    }
};
}

int main()
{
    usize stale = 0;
    for (usize stage = 0; stage != 2; ++stage)
        for (usize kind = 0; kind != 3; ++kind)
        {
            Image image(3); journal(image, 0); original_disk = image.blocks;
            Device device(image, 11), other_device(image, 11);
            Registry registry; Session session, foreign;
            const auto key = registry.register_device(device, 7), other = registry.register_device(other_device, 8);
            CHECK(key && other);
            CHECK(registry.acquire(key.value(), {11, device.sectors - 11}, block::SessionAccess::readonly, session));
            CHECK(registry.acquire(other.value(), {11, other_device.sectors - 11}, block::SessionAccess::readonly, foreign));
            auto *value = ::new (arena.data()) Preview{};
            AnchorMutation mutation{*value, foreign, kind}; device.context = &mutation; device.hook = AnchorMutation::apply;
            if (!stage) device.hook_geometry = 2; else device.hook_read = 1;
            const auto status = value->prepare(session, image.f.binding);
            const auto actual = session.report(), untouched = foreign.report(); CHECK(actual && untouched);
            CHECK(!status && actual.value().state == block::DeviceState::quarantined &&
                actual.value().reads == 1 && !actual.value().completed_reads && !actual.value().in_flight);
            CHECK(untouched.value().state == block::DeviceState::owned && !untouched.value().reads && !other_device.reads);
            CHECK(device.reads == stage && !device.writes && !device.flushes && image.blocks == original_disk);
            const auto report = value->report();
            const bool exact = reclaim_history::same_session(report.observed, actual.value());
            if (!exact) ++stale;
            std::printf("ANCHOR stage=%zu field=%zu original_state=%u observed_state=%u original_in_flight=%u "
                "observed_in_flight=%u exact=%u original_reads=%zu observed_reads=%zu original_identity=%llu observed_identity=%llu\n",
                stage, kind, u32(actual.value().state), u32(report.observed.state), u32(actual.value().in_flight),
                u32(report.observed.in_flight), u32(exact), actual.value().reads, report.observed.reads,
                static_cast<unsigned long long>(actual.value().identity), static_cast<unsigned long long>(report.observed.identity));
            value->~Preview(); CHECK(foreign.close());
        }
    std::printf("ANCHOR_OBSERVATION probe_cases=6 stale_final_observations=%zu foreign_reads=0 foreign_quarantine=0\n", stale);
    return stale ? 1 : 0;
}
