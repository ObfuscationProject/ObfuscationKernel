#include "ok/fs/sysfs.hpp"

namespace ok::fs
{
namespace
{
using AttributeKind = CpuSysFileSystem::AttributeKind;
constexpr std::array<std::string_view, CpuSysFileSystem::attribute_count> attribute_names{
    "kernel_max", "possible", "present", "online", "offline"};
constexpr std::array<std::string_view, 16> cpu_names{"cpu0", "cpu1", "cpu2", "cpu3", "cpu4", "cpu5",
                                                     "cpu6", "cpu7", "cpu8", "cpu9", "cpu10", "cpu11",
                                                     "cpu12", "cpu13", "cpu14", "cpu15"};
static_assert(cpu_names.size() == smp::max_cpus);
// Linux reports a page for every sysfs attribute.
constexpr usize attribute_size = 4096;
// Fixed inodes: the directories, the five attributes, then cpuN/ and cpuN/online.
constexpr u64 root_inode = 1, devices_inode = 2, system_inode = 3, cpu_inode = 4, attribute_inode = 5;
constexpr u64 cpu_directory_inode = 16, cpu_online_inode = 32;

// Fixed epoch timestamps with kernel provenance; the VFS supplies the device.
void kernel_attributes(FileAttributes &attributes, u64 inode)
{
    attributes.inode = inode;
    attributes.mark(Attribute::inode);
    attributes.mark(Attribute::blocks512);
    for (auto *time : {&attributes.access, &attributes.modification, &attributes.change})
        *time = {.seconds = 0, .nanoseconds = 0, .source = TimestampSource::kernel_fixed};
    attributes.mark(Attribute::access_time);
    attributes.mark(Attribute::modification_time);
    attributes.mark(Attribute::change_time);
}
// Two links plus one for each subdirectory's "..", as on Linux.
Metadata directory_metadata(u64 inode, u32 mode, u32 subdirectories)
{
    Metadata result{.type = NodeType::directory, .size = 0, .mode = mode_for(NodeType::directory, mode), .uid = 0,
                    .gid = 0, .link_count = 2 + subdirectories, .block_size = metadata_block_size, .blocks = 0};
    kernel_attributes(result.attributes, inode);
    return result;
}
u32 count(u32 mask)
{
    u32 total = 0;
    for (; mask; mask &= mask - 1) ++total;
    return total;
}
// Strict "cpuN": no sign, no leading zero, below max_cpus.
bool cpu_index(std::string_view name, u16 &index)
{
    for (usize cpu = 0; cpu < cpu_names.size(); ++cpu)
        if (cpu_names[cpu] == name)
        {
            index = static_cast<u16>(cpu);
            return true;
        }
    return false;
}
} // namespace

std::string_view CpuSysFileSystem::Directory::name() const
{
    switch (kind_)
    {
    case DirectoryKind::root: return "/";
    case DirectoryKind::devices: return "devices";
    case DirectoryKind::system: return "system";
    case DirectoryKind::cpu: return "cpu";
    case DirectoryKind::cpu_n: break;
    }
    return cpu_names[index_ < cpu_names.size() ? index_ : 0];
}

Metadata CpuSysFileSystem::Directory::metadata() const
{
    switch (kind_)
    {
    case DirectoryKind::root: return directory_metadata(root_inode, 0755, 1);
    case DirectoryKind::devices: return directory_metadata(devices_inode, 0755, 1);
    case DirectoryKind::system: return directory_metadata(system_inode, 0755, 1);
    case DirectoryKind::cpu: return directory_metadata(cpu_inode, 0755, count(owner_.view().present));
    case DirectoryKind::cpu_n: break;
    }
    return directory_metadata(cpu_directory_inode + index_, 0555, 0);
}

Result<DirectoryListing> CpuSysFileSystem::Directory::list(DirectoryReadIntent) const
{
    Result<DirectoryListing> result{std::in_place};
    auto &listing = result.value();
    auto add = [&](const Node &node) -> Status {
        auto &entry = listing.entries[listing.count++];
        entry.metadata = node.metadata();
        return entry.name.assign(node.name());
    };
    Status status{};
    switch (kind_)
    {
    case DirectoryKind::root: status = add(owner_.devices_); break;
    case DirectoryKind::devices: status = add(owner_.system_); break;
    case DirectoryKind::system: status = add(owner_.cpu_); break;
    case DirectoryKind::cpu:
        for (const auto &attribute : owner_.attributes_)
            if (status) status = add(attribute);
        for (u16 cpu = 0; cpu < smp::max_cpus; ++cpu)
            if (status && owner_.present(cpu)) status = add(owner_.cpus_[cpu]);
        break;
    case DirectoryKind::cpu_n:
        if (index_ != 0) status = add(owner_.cpu_online_[index_]);
        break;
    }
    if (!status) return status;
    return result;
}

FixedString<96> CpuSysFileSystem::Directory::path() const
{
    FixedString<96> text;
    auto append = [&](std::string_view part) {
        static_cast<void>(text.append('/'));
        for (char value : part) static_cast<void>(text.append(value));
    };
    if (kind_ == DirectoryKind::root) return text;
    append("devices");
    if (kind_ == DirectoryKind::devices) return text;
    append("system");
    if (kind_ == DirectoryKind::system) return text;
    append("cpu");
    if (kind_ == DirectoryKind::cpu) return text;
    append(name());
    return text;
}

// Relative to `root` when it is this directory or one of its ancestors here.
Result<FixedString<96>> CpuSysFileSystem::Directory::directory_path(const Node &root) const
{
    const Directory *base = nullptr;
    for (const auto *candidate : {&owner_.root_, &owner_.devices_, &owner_.system_, &owner_.cpu_})
        if (&root == candidate) base = candidate;
    if (&root == this) base = this;
    if (!base) return Status::not_found("directory is outside this root");
    const auto full = path();
    const auto prefix = base->path();
    if (full.view().substr(0, prefix.view().size()) != prefix.view())
        return Status::not_found("directory is outside this root");
    FixedString<96> relative;
    if (auto status = relative.assign(full.view().substr(prefix.view().size())); !status) return status;
    return relative;
}

Node *CpuSysFileSystem::Directory::lookup(std::string_view child)
{
    switch (kind_)
    {
    case DirectoryKind::root:
        return child == "devices" ? &owner_.devices_ : child == ".." ? this : nullptr;
    case DirectoryKind::devices:
        return child == "system" ? &owner_.system_ : child == ".." ? &owner_.root_ : nullptr;
    case DirectoryKind::system:
        return child == "cpu" ? &owner_.cpu_ : child == ".." ? &owner_.devices_ : nullptr;
    case DirectoryKind::cpu:
    {
        if (child == "..") return &owner_.system_;
        for (auto &attribute : owner_.attributes_)
            if (attribute.name() == child) return &attribute;
        u16 index{};
        return cpu_index(child, index) && owner_.present(index) ? &owner_.cpus_[index] : nullptr;
    }
    case DirectoryKind::cpu_n:
        if (child == "..") return &owner_.cpu_;
        return index_ != 0 && child == "online" ? &owner_.cpu_online_[index_] : nullptr;
    }
    return nullptr;
}

std::string_view CpuSysFileSystem::Attribute::name() const
{
    return kind_ == AttributeKind::cpu_online ? "online" : attribute_names[static_cast<usize>(kind_)];
}

Metadata CpuSysFileSystem::Attribute::metadata() const
{
    const bool control = kind_ == AttributeKind::cpu_online;
    Metadata result{.type = NodeType::regular, .size = attribute_size,
                    .mode = mode_for(NodeType::regular, control ? 0644u : 0444u), .uid = 0, .gid = 0,
                    .link_count = 1, .block_size = metadata_block_size, .blocks = 0};
    kernel_attributes(result.attributes,
                      control ? cpu_online_inode + index_ : attribute_inode + static_cast<u64>(kind_));
    return result;
}

CpuList CpuSysFileSystem::Attribute::content() const
{
    const auto view = owner_.view();
    switch (kind_)
    {
    case AttributeKind::kernel_max:
    {
        CpuList list{};
        constexpr auto text = std::string_view{"15\n"};
        static_assert(smp::max_cpus == 16, "kernel_max is the highest possible CPU number");
        for (char value : text) list.text[list.size++] = value;
        return list;
    }
    case AttributeKind::possible:
    case AttributeKind::present: return cpulist(view.present);
    case AttributeKind::online: return cpulist(view.online & view.present);
    case AttributeKind::offline: return cpulist(view.present & ~view.online);
    case AttributeKind::cpu_online: break;
    }
    CpuList list{};
    list.text[list.size++] = (view.online & (u32{1} << index_)) != 0 ? '1' : '0';
    list.text[list.size++] = '\n';
    return list;
}

Result<FileBuffer> CpuSysFileSystem::Attribute::read(usize offset, usize count) const
{
    const auto text = content();
    FileBuffer bytes{};
    for (usize i = offset; i < text.size && bytes.size < count && bytes.size < bytes.data.size(); ++i)
        bytes.data[bytes.size++] = static_cast<std::byte>(text.text[i]);
    return bytes;
}

// Writes reach the hotplug service through the open description; a node
// write is never a CPU request.
Status CpuSysFileSystem::Attribute::write(usize, std::span<const std::byte>)
{
    if (kind_ == AttributeKind::cpu_online)
        return Status::unsupported("CPU online writes go through the hotplug service");
    return Status::denied("sysfs attribute is read-only");
}

ControlNode CpuSysFileSystem::Attribute::control_node() const
{
    if (kind_ == AttributeKind::cpu_online) return {.kind = ControlNodeKind::cpu_online, .index = index_};
    return {.kind = ControlNodeKind::attribute};
}
} // namespace ok::fs
