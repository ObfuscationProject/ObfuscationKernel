#pragma once
#include "ok/arch/execution.hpp"
#include "ok/core/fixed.hpp"
#include "ok/memory/memory.hpp"
#include <array>
namespace ok::fs
{
class Node;
}
namespace ok::sched
{
inline constexpr usize max_process_children = 16;
inline constexpr usize max_memory_areas = 16;
inline constexpr usize max_process_fds = 32;
inline constexpr usize max_elf_load_segments = 8;

struct SignalState
{
    u64 pending{0};
    u64 blocked{0};
};

struct ProcessGroup
{
    u64 id{0};
};

struct Session
{
    u64 id{0};
};

inline constexpr usize max_open_descriptions = 256;
inline constexpr u16 invalid_description = 0xffffu;

struct FileDescriptor
{
    bool used{false};
    bool close_on_exec{false};
    u16 description{invalid_description};
};

struct OpenFileDescription
{
    bool used{false};
    bool readable{false};
    bool writable{false};
    bool console{false};
    bool input_console{false};
    u32 flags{0};
    usize offset{0};
    usize references{0};
    u16 pipe{invalid_description};
    fs::Node *node{nullptr};
    FixedString<96> path{};
};

class FileDescriptorTable final
{
  public:
    using FdLike = i32;
    // Legacy metadata helpers cannot release or copy scheduler-owned descriptions.
    Status open_slot(FdLike fd);
    Status close_slot(FdLike fd);
    Status clone_from(const FileDescriptorTable &source);
    [[nodiscard]] bool contains(FdLike fd) const;
    [[nodiscard]] usize open_count() const;
    [[nodiscard]] FileDescriptor *entry(FdLike fd)
    {
        return fd >= 0 && static_cast<usize>(fd) < entries_.size() ? &entries_[static_cast<usize>(fd)] : nullptr;
    }
    [[nodiscard]] const FileDescriptor *entry(FdLike fd) const
    {
        return fd >= 0 && static_cast<usize>(fd) < entries_.size() ? &entries_[static_cast<usize>(fd)] : nullptr;
    }
    [[nodiscard]] auto &entries()
    {
        return entries_;
    }
    [[nodiscard]] const auto &entries() const
    {
        return entries_;
    }

  private:
    std::array<FileDescriptor, max_process_fds> entries_{};
};

class MemoryMap final
{
  public:
    Status add_area(memory::VmArea area);
    Status clone_from(const MemoryMap &source);
    Status replace_with(memory::VmArea area);
    [[nodiscard]] bool contains(uptr address, usize length) const;
    [[nodiscard]] bool allows_user_access(uptr address, usize length, usize permissions) const;
    [[nodiscard]] Status require_user_access(uptr address, usize length, usize permissions) const;
    [[nodiscard]] usize area_count() const
    {
        return areas_.size();
    }
    [[nodiscard]] const memory::VmArea *area(usize index) const;

  private:
    StaticVector<memory::VmArea, max_memory_areas> areas_;
};

struct MemoryMapping
{
    bool used{false};
    uptr address{0};
    usize length{0};
    u32 protection{0};
    u32 flags{0};
    i32 fd{-1};
    usize offset{0};
};

struct ProcessResources
{
    arch::execution::AddressSpace native_space{};
    StaticVector<arch::execution::PageMapping, 128> native_pages{};
    FileDescriptorTable files{};
    MemoryMap memory{};
    std::array<MemoryMapping, 32> mappings{};
    FixedString<96> cwd{"/"};
    uptr program_break_base{0x1000'0000};
    uptr program_break{0x1000'0000};
    uptr program_break_limit{0x1010'0000};
    uptr next_mapping_address{0x2000'0000};
    u32 file_mode_mask{0022};
};

inline constexpr usize max_process_pipes = 16;
inline constexpr usize pipe_capacity = 4096;
struct PipeBuffer
{
    bool used{false};
    usize head{0};
    usize size{0};
    std::array<std::byte, pipe_capacity> data{};
};
enum class ThreadWaitReason : u8
{
    none,
    child,
    pipe_read,
    pipe_write
};

struct ThreadLocalState
{
    uptr fs_base{0};
    uptr gs_base{0};
    uptr clear_tid_address{0};
    uptr robust_list{0};
    usize robust_list_length{0};
};

} // namespace ok::sched
