#pragma once

#include "ok/fs/vfs.hpp"
#include "ok/smp/hotplug.hpp"

#include <array>
#include <string_view>
#include <utility>

namespace ok::fs
{
// The CPU part of Linux sysfs (docs/modules/sysfs.md). Where it is mounted
// (normally /sys) is distribution policy; the tree is
//
//   devices/system/cpu/kernel_max         0444  "15\n"
//   devices/system/cpu/possible, present  0444  started CPUs, as a cpulist
//   devices/system/cpu/online             0444  CPUs online now
//   devices/system/cpu/offline            0444  present and not online
//   devices/system/cpu/cpuN/              0555  every present N, cpu0 included
//   devices/system/cpu/cpuN/online        0644  N >= 1: "1\n" or "0\n"
//
// The nodes are fixed and live as long as the object. Open follows kernfs:
// a 0444 attribute opened for writing is EACCES even for root, the
// directories refuse creation (EACCES), and a write to cpuN/online goes to
// the CPU hotplug service through the description's control node; the node
// itself never performs one.

// A Linux cpulist ("0-3,5\n"); "\n" for an empty mask.
struct CpuList
{
    std::array<char, 24> text{};
    usize size{};
    [[nodiscard]] constexpr std::string_view view() const { return {text.data(), size}; }
};

[[nodiscard]] constexpr CpuList cpulist(u32 mask)
{
    CpuList list{};
    auto put = [&](char value) { list.text[list.size++] = value; };
    auto number = [&](u32 value) {
        if (value >= 10) put(static_cast<char>('0' + value / 10));
        put(static_cast<char>('0' + value % 10));
    };
    bool first = true;
    for (u32 cpu = 0; cpu < smp::max_cpus;)
    {
        if ((mask & (u32{1} << cpu)) == 0)
        {
            ++cpu;
            continue;
        }
        u32 last = cpu;
        while (last + 1 < smp::max_cpus && (mask & (u32{1} << (last + 1))) != 0) ++last;
        if (!first) put(',');
        first = false;
        number(cpu);
        if (last != cpu)
        {
            put('-');
            number(last);
        }
        cpu = last + 1;
    }
    put('\n');
    return list;
}
static_assert(cpulist(0).view() == "\n" && cpulist(1).view() == "0\n" && cpulist(0xf).view() == "0-3\n");
static_assert(cpulist(0b1101).view() == "0,2-3\n" && cpulist(0xffff).view() == "0-15\n");
// The longest list of sixteen CPUs.
static_assert(cpulist(0x5555).view() == "0,2,4,6,8,10,12,14\n" && cpulist(0x5555).size == 19);
static_assert(cpulist(0xaaaa).view() == "1,3,5,7,9,11,13,15\n");

class CpuSysFileSystem final
{
  public:
    using ViewSource = smp::CpuView (*)() noexcept;
    enum class AttributeKind : u8 { kernel_max, possible, present, online, offline, cpu_online };
    static constexpr usize attribute_count = 5; // kernel_max .. offline in devices/system/cpu.

    // Constant-initialized: kernel images run no dynamic initializers, so a
    // global instance must not need one. Hosted models supply their own view.
    constexpr CpuSysFileSystem() : CpuSysFileSystem(&smp::cpu_view) {}
    constexpr explicit CpuSysFileSystem(ViewSource source)
        : CpuSysFileSystem(source, std::make_index_sequence<smp::max_cpus>{})
    {
    }
    CpuSysFileSystem(const CpuSysFileSystem &) = delete;
    CpuSysFileSystem &operator=(const CpuSysFileSystem &) = delete;
    CpuSysFileSystem(CpuSysFileSystem &&) = delete;
    CpuSysFileSystem &operator=(CpuSysFileSystem &&) = delete;

    [[nodiscard]] Node &root() { return root_; }
    [[nodiscard]] smp::CpuView view() const { return source_(); }

  private:
    enum class DirectoryKind : u8 { root, devices, system, cpu, cpu_n };

    template <usize... Index>
    constexpr CpuSysFileSystem(ViewSource source, std::index_sequence<Index...>)
        : source_(source), root_(*this, DirectoryKind::root), devices_(*this, DirectoryKind::devices),
          system_(*this, DirectoryKind::system), cpu_(*this, DirectoryKind::cpu),
          cpus_{Directory{*this, DirectoryKind::cpu_n, static_cast<u16>(Index)}...},
          attributes_{Attribute{*this, AttributeKind::kernel_max}, Attribute{*this, AttributeKind::possible},
                      Attribute{*this, AttributeKind::present}, Attribute{*this, AttributeKind::online},
                      Attribute{*this, AttributeKind::offline}},
          cpu_online_{Attribute{*this, AttributeKind::cpu_online, static_cast<u16>(Index)}...}
    {
    }

    class Directory final : public Node
    {
      public:
        constexpr Directory(CpuSysFileSystem &owner, DirectoryKind kind, u16 index = 0)
            : owner_(owner), kind_(kind), index_(index)
        {
        }
        std::string_view name() const override;
        Metadata metadata() const override;
        Result<FileBuffer> read(usize, usize) const override { return Status::is_directory("sysfs directory"); }
        Status write(usize, std::span<const std::byte>) override { return Status::is_directory("sysfs directory"); }
        Result<DirectoryListing> list(DirectoryReadIntent = DirectoryReadIntent::access) const override;
        const void *filesystem_identity() const override { return &owner_; }
        Result<FixedString<96>> directory_path(const Node &root) const override;
        Node *lookup(std::string_view child) override;
        Status create(std::string_view, NodeType) override { return Status::denied("sysfs directories are fixed"); }
        Result<Node *> create_child(std::string_view, const InitialNodeAttributes &) override
        {
            return Status::denied("sysfs directories are fixed");
        }
        Status create_symlink(std::string_view, std::string_view, u32, u32) override
        {
            return Status::denied("sysfs directories are fixed");
        }
        Status chmod(u32) override { return Status::permission("sysfs nodes are fixed"); }
        Status chown(u32, u32) override { return Status::permission("sysfs nodes are fixed"); }
        // The path of this directory below the filesystem root ("" for it).
        [[nodiscard]] FixedString<96> path() const;

      private:
        CpuSysFileSystem &owner_;
        DirectoryKind kind_;
        u16 index_;
    };
    class Attribute final : public Node
    {
      public:
        constexpr Attribute(const CpuSysFileSystem &owner, AttributeKind kind, u16 index = 0)
            : owner_(owner), kind_(kind), index_(index)
        {
        }
        std::string_view name() const override;
        Metadata metadata() const override;
        Result<FileBuffer> read(usize offset, usize count) const override;
        Status write(usize, std::span<const std::byte>) override;
        Status truncate(usize) override { return Status::success(); }
        const void *filesystem_identity() const override { return &owner_; }
        ControlNode control_node() const override;
        Node *lookup(std::string_view) override { return nullptr; }
        Status create(std::string_view, NodeType) override { return Status::not_directory("sysfs attribute"); }
        Status chmod(u32) override { return Status::permission("sysfs nodes are fixed"); }
        Status chown(u32, u32) override { return Status::permission("sysfs nodes are fixed"); }
        // The attribute's current text.
        [[nodiscard]] CpuList content() const;

      private:
        const CpuSysFileSystem &owner_;
        AttributeKind kind_;
        u16 index_;
    };

    [[nodiscard]] bool present(u16 cpu) const { return cpu < smp::max_cpus && (view().present & (u32{1} << cpu)) != 0; }

    ViewSource source_;
    Directory root_, devices_, system_, cpu_;
    std::array<Directory, smp::max_cpus> cpus_;
    std::array<Attribute, attribute_count> attributes_;
    std::array<Attribute, smp::max_cpus> cpu_online_;
};
} // namespace ok::fs
