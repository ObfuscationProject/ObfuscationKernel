#pragma once

#include "ok/fs/ext4_mounted_metadata.hpp"
#include "ok/sched/scheduler.hpp"
#include <new>
#include <type_traits>

namespace ok::tests
{
namespace mount_retention_model
{
class ObservedNode final : public fs::Node
{
  public:
    std::string_view name() const override { ++calls; return "/"; }
    fs::Metadata metadata() const override { ++calls; return {}; }
    Result<fs::FileBuffer> read(usize, usize) const override
    { ++calls; return Status::unsupported(); }
    Status write(usize, std::span<const std::byte>) override { ++calls; return Status::unsupported(); }
    fs::Node *lookup(std::string_view) override { ++calls; return nullptr; }
    Status create(std::string_view, fs::NodeType) override { ++calls; return Status::unsupported(); }
    Status chmod(u32) override { ++calls; return Status::unsupported(); }
    Status chown(u32, u32) override { ++calls; return Status::unsupported(); }
    Status retain() override { ++calls; return {}; }
    Status release() override { ++calls; return {}; }
    mutable usize calls{};
};
struct Fixture
{
    ObservedNode node;
    fs::Ext4MountedMetadata<1> provider;
    fs::VirtualFileSystem vfs;
    sched::Scheduler scheduler;
};
static_assert(!std::is_copy_constructible_v<fs::MountedRootReference>);
static_assert(std::is_move_constructible_v<fs::MountedRootReference>);
static_assert(!std::is_move_assignable_v<fs::MountedRootReference>);
static_assert(!std::is_copy_constructible_v<fs::VirtualFileSystem>);
}

// Refusal model only. Positive mounted-media lifecycle is qualified separately.
inline Status check_mount_retention_refusal_model()
{
    using namespace mount_retention_model;
    alignas(Fixture) static std::array<std::byte, sizeof(Fixture)> storage{};
    auto &fixture = *::new (storage.data()) Fixture;
    const auto result = [&]() -> Status {
        fs::MountedRootReference empty;
        fs::MountedRootReference moved{std::move(empty)};
        if (empty.valid() || moved.valid() || empty.release().code() != StatusCode::bad_descriptor ||
            moved.clone().status().code() != StatusCode::bad_descriptor)
            return Status::fault("mount retention model invalid token authority");
        if (fixture.provider.retain_root() || fixture.provider.begin_drain() || fixture.provider.close())
            return Status::fault("mount retention model unused provider admitted");
        if (fixture.vfs.mount_owned("/", std::move(moved)).status().code() != StatusCode::bad_descriptor ||
            fixture.vfs.detach({}).code() != StatusCode::bad_descriptor)
            return Status::fault("mount retention model invalid attachment admitted");
        const fs::PathHandle stale{&fixture.node, 99};
        for (usize variant = 0; variant < 2; ++variant)
        {
            fs::ResolveContext context;
            if (variant) context.start = stale;
            else context.root = stale;
            if (fixture.vfs.resolve(context, "/").status().code() != StatusCode::not_found || fixture.node.calls)
                return Status::fault("mount retention model dereferenced unavailable context");
        }
        if (fixture.vfs.directory_path(fixture.node, stale.mount_id).status().code() != StatusCode::not_found ||
            fixture.node.calls)
            return Status::fault("mount retention model reconstructed unavailable context");
        if (auto status = fixture.vfs.create("/run", fs::NodeType::directory); !status) return status;
        auto runtime = fixture.vfs.mount_ram_owned("/run");
        if (!runtime) return runtime.status();
        const std::array<fs::OwnedMountExpectation, 1> expected{{
            {runtime.value(), "/run", fs::OwnedMountKind::ram, {}}}};
        if (fixture.vfs.preflight_owned_mounts({}).code() != StatusCode::unsupported ||
            !fixture.vfs.preflight_owned_mounts(expected) || !fixture.vfs.shutdown_reference_status())
            return Status::fault("mount inventory exact-owner refusal model failed");
        auto retained = fixture.vfs.resolve({}, "/run");
        if (!retained || fixture.vfs.shutdown_reference_status().code() != StatusCode::busy ||
            !fixture.vfs.detach(runtime.value()) ||
            fixture.vfs.preflight_owned_mounts(expected).code() != StatusCode::bad_descriptor ||
            fixture.vfs.shutdown_reference_status().code() != StatusCode::busy ||
            !retained.value().release() || !fixture.vfs.shutdown_reference_status())
            return Status::fault("shutdown reference inspection lost a detached RAM owner");
        if (auto status = fixture.scheduler.drain_all_resources(); !status) return status;
        return fixture.scheduler.drain_all_resources();
    }();
    fixture.~Fixture();
    return result;
}
} // namespace ok::tests
