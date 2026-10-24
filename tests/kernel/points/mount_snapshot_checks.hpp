#pragma once

#include "ok/fs/vfs.hpp"
#include <new>

namespace ok::tests
{
namespace mount_snapshot_model
{
inline bool same(const fs::MountSnapshot &left, const fs::MountSnapshot &right)
{
    return left.generation == right.generation && left.slot == right.slot && left.state == right.state &&
           left.parent_id == right.parent_id && left.path_length == right.path_length && left.path == right.path;
}

inline bool matches(const fs::MountSnapshot &row, u32 slot, u32 generation,
                    fs::MountSnapshotState state, u32 parent, std::string_view path)
{
    if (row.slot != slot || row.generation != generation || row.state != state || row.parent_id != parent ||
        row.path_length != path.size() || std::string_view(row.path.data(), row.path_length) != path)
        return false;
    for (usize index = path.size(); index < row.path.size(); ++index)
        if (row.path[index]) return false;
    return true;
}

class ObservedNode final : public fs::Node
{
  public:
    std::string_view name() const override { ++calls; return "/"; }
    fs::Metadata metadata() const override
    { ++calls; return {.type = fs::NodeType::directory, .mode = fs::mode_for(fs::NodeType::directory, 0755)}; }
    Result<fs::Metadata> query_metadata() const override
    {
        ++calls;
        if (probe)
        {
            fs::MountSnapshot output{};
            output.path.fill('!');
            const auto before = output;
            refused = vfs->mount_snapshot(0, output).code() == StatusCode::busy && same(output, before);
        }
        return metadata();
    }
    Result<fs::FileBuffer> read(usize, usize) const override { ++calls; return Status::unsupported(); }
    Status write(usize, std::span<const std::byte>) override { ++calls; return Status::unsupported(); }
    fs::Node *lookup(std::string_view) override { ++calls; return nullptr; }
    Status create(std::string_view, fs::NodeType) override { ++calls; return Status::unsupported(); }
    Status chmod(u32) override { ++calls; return Status::unsupported(); }
    Status chown(u32, u32) override { ++calls; return Status::unsupported(); }
    Status retain() override { ++calls; return {}; }
    Status release() override { ++calls; return {}; }
    fs::VirtualFileSystem *vfs{};
    mutable usize calls{};
    bool probe{};
    mutable bool refused{};
};

struct Fixture
{
    ObservedNode node;
    fs::VirtualFileSystem vfs;
};
} // namespace mount_snapshot_model

// Namespace observation model only; no device state, reference drain or seal.
inline Status check_mount_snapshot_model()
{
    using namespace mount_snapshot_model;
    alignas(Fixture) static std::array<std::byte, sizeof(Fixture)> storage{};
    auto &fixture = *::new (storage.data()) Fixture;
    const auto result = [&]() -> Status {
        fs::MountSnapshot output{};
        if (!fixture.vfs.mount_snapshot(0, output) ||
            !matches(output, 0, 1, fs::MountSnapshotState::underlying_ram, 0, "/"))
            return Status::fault("mount snapshot underlying namespace model failed");
        if (auto status = fixture.vfs.create("/run", fs::NodeType::directory); !status) return status;
        auto runtime = fixture.vfs.mount_ram_owned("/run");
        if (!runtime) return runtime.status();
        if (!fixture.vfs.mount_snapshot(1, output) ||
            !matches(output, 1, 2, fs::MountSnapshotState::owned_ram, 0, "/run"))
            return Status::fault("mount snapshot RAM attachment model failed");
        if (auto status = fixture.vfs.create("/borrowed", fs::NodeType::directory); !status) return status;
        if (auto status = fixture.vfs.mount_borrowed("/borrowed", fixture.node); !status) return status;
        const auto calls = fixture.node.calls;
        if (!fixture.vfs.mount_snapshot(2, output) ||
            !matches(output, 2, 3, fs::MountSnapshotState::borrowed_provider, 0, "/borrowed") ||
            fixture.node.calls != calls)
            return Status::fault("mount snapshot called a provider during observation");
        fixture.node.vfs = &fixture.vfs;
        fixture.node.probe = true;
        if (!fixture.vfs.stat("/borrowed") || !fixture.node.refused)
            return Status::fault("mount snapshot active operation model failed");
        fixture.node.probe = false;
        if (auto status = fixture.vfs.detach(runtime.value()); !status) return status;
        if (!fixture.vfs.mount_snapshot(1, output) ||
            !matches(output, 1, 4, fs::MountSnapshotState::detached, 0, "/run"))
            return Status::fault("mount snapshot detached history model failed");
        for (u32 slot = 3; slot < fs::mount_snapshot_capacity; ++slot)
            if (!fixture.vfs.mount_snapshot(slot, output) ||
                !matches(output, slot, 4, fs::MountSnapshotState::unused, 0, ""))
                return Status::fault("mount snapshot unused row model failed");
        const auto before = output;
        if (fixture.vfs.mount_snapshot(fs::mount_snapshot_capacity, output).code() != StatusCode::invalid_argument ||
            !same(output, before))
            return Status::fault("mount snapshot invalid selector changed output");
        return {};
    }();
    fixture.~Fixture();
    return result;
}
} // namespace ok::tests
