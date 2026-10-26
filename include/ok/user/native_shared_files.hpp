#pragma once
#include "ok/core/fixed.hpp"
#include "ok/fs/attributes.hpp"
#include "ok/user/native_page_pool.hpp"
#include "ok/user/native_shared_file_handle.hpp"
#include <array>
#include <utility>

namespace ok::user::native
{
// Serialized boot-CPU provider. File pages and each alias have distinct pool
// references; allocator generations remain authoritative throughout retirement.
class SharedMemoryFiles final : private NonCopyable
{
  public:
    static constexpr usize capacity = 16, max_pages = 64;
    static constexpr usize page_size = NativePagePool::page_size;
    static constexpr u32 seal_seal = 1, seal_shrink = 2, seal_grow = 4, seal_write = 8;
    static constexpr u32 supported_seals = seal_seal | seal_shrink | seal_grow | seal_write;

    class Reservation final
    {
      public:
        Reservation(const Reservation &) = delete;
        Reservation &operator=(const Reservation &) = delete;
        Reservation &operator=(Reservation &&) = delete;
        Reservation(Reservation &&other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), handle_(other.handle_) {}
        ~Reservation() { cancel(); }
        [[nodiscard]] SharedFileHandle handle() const { return handle_; }
        [[nodiscard]] Result<SharedFileHandle> publish()
        {
            if (!owner_) return Status::bad_descriptor("shared file reservation completed");
            auto *file = owner_->reserved(handle_);
            if (!file) return Status::bad_descriptor("shared file reservation is stale");
            file->state = State::live;
            file->description = true;
            owner_ = nullptr;
            return handle_;
        }
        void cancel()
        {
            if (!owner_) return;
            if (auto *file = owner_->reserved(handle_)) *file = {};
            owner_ = nullptr;
        }
      private:
        friend class SharedMemoryFiles;
        Reservation(SharedMemoryFiles &owner, SharedFileHandle handle) : owner_(&owner), handle_(handle) {}
        SharedMemoryFiles *owner_{};
        SharedFileHandle handle_{};
    };

    struct FileInfo
    {
        usize size{}, pages{}, aliases{};
        u32 seals{};
        bool description{};
        u32 uid{}, gid{};
        u64 inode{};
    };
    struct LedgerInfo
    {
        usize files{}, aliases{}, retiring_files{}, retained_frames{}, reserved_cells{};
        Status first_cleanup_failure{};
    };

    Status bind(NativePagePool &pool)
    {
        if (pool_ && pool_ != &pool) return Status::busy("shared file page owner cannot change");
        if (pool.source() == NativePagePool::Source::inactive)
            return Status::not_initialized("shared file page owner is inactive");
        pool_ = &pool;
        return {};
    }
    [[nodiscard]] bool ready() const { return pool_ && pool_->source() != NativePagePool::Source::inactive; }
    [[nodiscard]] Result<Reservation> reserve(std::string_view name, u32 uid = 0, u32 gid = 0)
    {
        if (!pool_) return Status::not_initialized("shared file page owner is unavailable");
        if (name.size() > 249) return Status::name_too_long("shared file name exceeds bound");
        for (usize index = 0; index < files_.size(); ++index)
            if (files_[index].state == State::free && generations_[index] != ~u64{0})
            {
                auto &file = files_[index];
                if (auto status = file.name.assign(name); !status) return status;
                auto inode = inode_sequence_.next();
                if (!inode) return inode.status();
                file.state = State::reserved;
                file.uid = uid; file.gid = gid; file.inode = inode.value();
                const SharedFileHandle handle{static_cast<u64>(reinterpret_cast<uptr>(this)),
                    ++generations_[index], static_cast<u16>(index)};
                return Reservation{*this, handle};
            }
        return Status::system_file_limit("shared file object table is full");
    }
    [[nodiscard]] bool valid(SharedFileHandle handle) const { return live(handle) != nullptr; }
    [[nodiscard]] Result<FileInfo> info(SharedFileHandle handle) const
    {
        const auto *file = live(handle);
        if (!file) return Status::bad_descriptor("shared file handle is foreign or stale");
        return FileInfo{file->size, file->pages, file->aliases, file->seals, file->description,
            file->uid, file->gid, file->inode};
    }
    Status add_seals(SharedFileHandle handle, u32 seals)
    {
        auto *file = live(handle);
        if (!file) return Status::bad_descriptor("shared file handle is foreign or stale");
        if (seals & ~supported_seals) return Status::unsupported("shared file seal is not admitted");
        if (file->seals & seal_seal) return Status::permission("shared file seals are final");
        // Every pre-WRITE alias was admitted through an O_RDWR description and
        // can regain write permission, including current READ/NONE mappings.
        // Alias discharge can precede table retirement once exact-frame pins
        // are complete; those independent holds must also retire first.
        // An existing WRITE bit is idempotent with sealed read-only aliases.
        if ((seals & seal_write) && !(file->seals & seal_write))
        {
            if (file->aliases)
                return Status::busy("shared file write seal requires complete alias retirement");
            for (usize index = 0; index < file->pages; ++index)
            {
                auto pins = pool_->pin_count(file->frames[index]);
                if (!pins) return pins.status();
                if (pins.value())
                    return Status::busy("shared file write seal requires exact-frame pin retirement");
            }
        }
        // All refusal predicates precede this single combined-mask publication.
        file->seals |= seals;
        return {};
    }
    [[nodiscard]] Status check_write(SharedFileHandle handle, usize count) const
    {
        const auto *file = live(handle);
        if (!file) return Status::bad_descriptor("shared file handle is foreign or stale");
        // Linux zero-length writes are no-ops even with WRITE sealing; preserve
        // the canonical open-description offset as well as every file byte.
        if (count && (file->seals & seal_write))
            return Status::permission("shared file contents are write sealed");
        return {};
    }
    [[nodiscard]] Status check_mapping_write(const memory::PhysicalFrame &frame) const
    {
        const auto location = locate(frame);
        if (location.file == capacity)
            return Status::bad_descriptor("shared mapping lost its canonical file owner");
        if (files_[location.file].seals & seal_write)
            return Status::denied("shared file mapping cannot gain write permission");
        return {};
    }
    Status initialize_size(SharedFileHandle handle, usize size)
    {
        auto *file = live(handle);
        if (!file) return Status::bad_descriptor("shared file handle is foreign or stale");
        if (size == file->size) return {};
        if ((size < file->size && (file->seals & seal_shrink)) ||
            (size > file->size && (file->seals & seal_grow)))
            return Status::permission("shared file size is sealed");
        if (file->size) return Status::unsupported("shared file resize is not admitted");
        if (!size || size % page_size || size / page_size > max_pages)
            return Status::unsupported("shared file size must be within its complete-page bound");
        const usize count = size / page_size;
        auto reserved = reserve_size(count);
        if (!reserved) return reserved.status();
        auto reservation = std::move(reserved.value());
        usize acquired{};
        Status failure{};
        for (; acquired < count; ++acquired)
        {
#if defined(OK_ENABLE_TEST_POINTS)
            if (acquired == refuse_allocation_after_)
            {
                failure = Status::no_memory("injected shared file allocation refusal");
                break;
            }
#endif
            auto allocated = pool_->allocate_frame();
            if (!allocated) { failure = allocated.status(); break; }
            const auto frame = allocated.value();
            reservation.stage(acquired, frame);
            if (auto status = pool_->mark_shared(frame); !status)
            {
                ++acquired;
                failure = status;
                break;
            }
            auto *bytes = reinterpret_cast<std::byte *>(frame.address);
            for (usize offset = 0; offset < page_size; ++offset) bytes[offset] = std::byte{};
        }
        if (!failure) return failure;
        reservation.publish(*file, size);
        return {};
    }
    [[nodiscard]] Result<memory::PhysicalFrame> page(SharedFileHandle handle, usize index) const
    {
        const auto *file = live(handle);
        if (!file) return Status::bad_descriptor("shared file handle is foreign or stale");
        if ((file->seals & (seal_shrink | seal_grow)) != (seal_shrink | seal_grow))
            return Status::unsupported("shared file mapping requires size seals");
        if (index >= file->pages) return Status::unsupported("shared file mapping is beyond EOF");
        if (!valid_frame(file->frames[index])) return Status::fault("shared file lost its canonical page");
        return file->frames[index];
    }
    [[nodiscard]] bool owns(const memory::PhysicalFrame &frame) const
    {
        return locate(frame).file != capacity;
    }
    Status retain_alias(const memory::PhysicalFrame &frame)
    {
        const auto location = locate(frame);
        if (location.file == capacity) return Status::bad_descriptor("shared alias has no canonical file owner");
        auto &file = files_[location.file];
        if (file.aliases == ~usize{0} || file.page_aliases[location.page] == 65534)
            return Status::overflow("shared file alias references are exhausted");
        if ((file.seals & (seal_shrink | seal_grow)) != (seal_shrink | seal_grow))
            return Status::unsupported("shared file alias requires size seals");
        if (auto status = pool_->retain_shared(frame); !status) return status;
        ++file.aliases;
        ++file.page_aliases[location.page];
        return {};
    }
    Status release_alias(const memory::PhysicalFrame &frame)
    {
        bool consumed{};
        return release_alias(frame, consumed);
    }
    // Retiring canonical base pages can fail after the alias was discharged.
    // Callers retaining a retry ledger must distinguish these two outcomes.
    Status release_alias(const memory::PhysicalFrame &frame, bool &consumed)
    {
        consumed = false;
        const auto location = locate(frame);
        if (location.file == capacity) return Status::bad_descriptor("shared alias has no canonical file owner");
        auto &file = files_[location.file];
        if (!file.aliases || !file.page_aliases[location.page])
            return Status::bad_descriptor("shared file page has no alias reference");
        if (auto status = pool_->release_reference(frame); !status) return status;
        --file.aliases;
        --file.page_aliases[location.page];
        consumed = true;
        return !file.aliases && !file.description ? retire(file) : Status{};
    }
    Status release_description(SharedFileHandle handle)
    {
        auto *file = live(handle);
        if (!file || !file->description) return Status::bad_descriptor("shared file provider hold is stale");
        file->description = false;
        return file->aliases ? Status{} : retire(*file);
    }
    [[nodiscard]] Result<usize> read(SharedFileHandle handle, usize offset, std::span<std::byte> output) const
    {
        const auto *file = live(handle);
        if (!file) return Status::bad_descriptor("shared file handle is foreign or stale");
        const usize count = offset < file->size ?
            (output.size() < file->size - offset ? output.size() : file->size - offset) : 0;
        if (count)
            for (usize index = offset / page_size; index <= (offset + count - 1) / page_size; ++index)
                if (!valid_frame(file->frames[index])) return Status::fault("shared file read lost its canonical page");
        for (usize index = 0; index < count; ++index)
        {
            const auto &frame = file->frames[(offset + index) / page_size];
            output[index] = reinterpret_cast<const std::byte *>(frame.address)[(offset + index) % page_size];
        }
        return count;
    }
    [[nodiscard]] Result<usize> write(SharedFileHandle handle, usize offset, std::span<const std::byte> input)
    {
        auto *file = live(handle);
        if (!file) return Status::bad_descriptor("shared file handle is foreign or stale");
        if (input.empty()) return usize{0};
        if (auto status = check_write(handle, input.size()); !status) return status;
        if (offset >= file->size)
            return file->seals & seal_grow ? Status::permission("shared file write cannot grow sealed size")
                                         : Status::unsupported("shared file write cannot initialize or resize size");
        const usize count = input.size() < file->size - offset ? input.size() : file->size - offset;
        const usize first = offset / page_size, last = (offset + count - 1) / page_size;
        for (usize index = first; index <= last; ++index)
            if (!valid_frame(file->frames[index])) return Status::fault("shared file write lost its canonical page");
        for (usize index = 0; index < count; ++index)
            reinterpret_cast<std::byte *>(file->frames[(offset + index) / page_size].address)[(offset + index) % page_size] = input[index];
        return count;
    }
    Status retry_retirement()
    {
        Status result{};
        for (auto &cell : ledger_) if (cell.state == LedgerState::retained)
        {
            const auto status = pool_->release_reference(cell.frame);
            if (status) cell = {};
            else if (result) result = status;
        }
        for (auto &file : files_) if (file.state == State::retiring)
            if (auto status = retire(file); !status && result) result = status;
        return result;
    }
    [[nodiscard]] LedgerInfo ledger() const
    {
        LedgerInfo result{};
        result.first_cleanup_failure = first_cleanup_failure_;
        for (const auto &file : files_)
        {
            result.files += file.state != State::free;
            result.retiring_files += file.state == State::retiring;
            result.aliases += file.aliases;
        }
        for (const auto &cell : ledger_)
        {
            result.retained_frames += cell.state == LedgerState::retained;
            result.reserved_cells += cell.state == LedgerState::reserved;
        }
        return result;
    }
    [[nodiscard]] const memory::PhysicalFrame *retained_frame(usize index) const
    {
        return index < ledger_.size() && ledger_[index].state == LedgerState::retained ? &ledger_[index].frame : nullptr;
    }

  private:
#if defined(OK_ENABLE_TEST_POINTS)
    friend struct SharedMemoryFilesFaultProbe;
#endif
    enum class State : u8 { free, reserved, live, retiring };
    enum class LedgerState : u8 { free, reserved, retained };
    struct File
    {
        State state{};
        bool description{};
        usize size{}, pages{}, aliases{};
        u32 seals{};
        u32 uid{}, gid{};
        u64 inode{};
        FixedString<250> name{};
        std::array<memory::PhysicalFrame, max_pages> frames{};
        std::array<u16, max_pages> page_aliases{};
    };
    struct Cell
    {
        LedgerState state{};
        u64 reservation{};
        memory::PhysicalFrame frame{};
        Status failure{};
    };
    class SizeReservation final
    {
      public:
        SizeReservation(const SizeReservation &) = delete;
        SizeReservation &operator=(const SizeReservation &) = delete;
        SizeReservation &operator=(SizeReservation &&) = delete;
        SizeReservation(SizeReservation &&other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), cells_(other.cells_), count_(other.count_) {}
        ~SizeReservation()
        {
            if (!owner_) return;
            for (usize index = 0; index < count_; ++index)
            {
                auto &cell = owner_->ledger_[cells_[index]];
                if (cell.frame.owner)
                    if (auto released = owner_->release_staged(cell.frame); !released)
                    {
                        cell.state = LedgerState::retained;
                        cell.failure = released;
                        if (owner_->first_cleanup_failure_) owner_->first_cleanup_failure_ = released;
                        continue;
                    }
                cell = {};
            }
        }
        void stage(usize index, memory::PhysicalFrame frame) { owner_->ledger_[cells_[index]].frame = frame; }
        void publish(File &file, usize size)
        {
            for (usize index = 0; index < count_; ++index)
            {
                auto &cell = owner_->ledger_[cells_[index]];
                file.frames[index] = cell.frame;
                cell = {};
            }
            file.pages = count_;
            file.size = size;
            owner_ = nullptr;
        }
      private:
        friend class SharedMemoryFiles;
        SizeReservation(SharedMemoryFiles &owner, usize count, u64 identity) : owner_(&owner), count_(count)
        {
            usize held{};
            for (usize index = 0; index < owner.ledger_.size() && held < count; ++index)
                if (owner.ledger_[index].state == LedgerState::free)
                {
                    owner.ledger_[index].state = LedgerState::reserved;
                    owner.ledger_[index].reservation = identity;
                    cells_[held++] = static_cast<u16>(index);
                }
        }
        SharedMemoryFiles *owner_{};
        std::array<u16, max_pages> cells_{};
        usize count_{};
    };
    [[nodiscard]] Result<SizeReservation> reserve_size(usize count)
    {
        usize free{};
        for (const auto &cell : ledger_) free += cell.state == LedgerState::free;
        if (free < count) return Status::no_memory("shared file retirement ledger is full");
        if (size_reservations_ == ~u64{0}) return Status::overflow("shared file size reservation sequence exhausted");
        return SizeReservation{*this, count, ++size_reservations_};
    }
    struct Location { usize file{capacity}, page{}; };
    [[nodiscard]] bool matches(SharedFileHandle handle) const
    {
        return handle.owner == static_cast<u64>(reinterpret_cast<uptr>(this)) && handle.generation &&
            handle.slot < capacity && generations_[handle.slot] == handle.generation;
    }
    [[nodiscard]] File *reserved(SharedFileHandle handle)
    {
        return matches(handle) && files_[handle.slot].state == State::reserved ? &files_[handle.slot] : nullptr;
    }
    [[nodiscard]] File *live(SharedFileHandle handle)
    {
        return matches(handle) && files_[handle.slot].state == State::live ? &files_[handle.slot] : nullptr;
    }
    [[nodiscard]] const File *live(SharedFileHandle handle) const
    {
        return matches(handle) && files_[handle.slot].state == State::live ? &files_[handle.slot] : nullptr;
    }
    [[nodiscard]] static bool same_frame(const memory::PhysicalFrame &left, const memory::PhysicalFrame &right)
    {
        return left.owner == right.owner && left.index == right.index && left.address == right.address &&
            left.generation && left.generation == right.generation;
    }
    [[nodiscard]] bool valid_frame(const memory::PhysicalFrame &frame) const
    {
        const auto actual = pool_->handle(frame.address);
        return actual && same_frame(frame, *actual) && pool_->reference_count(frame.address);
    }
    [[nodiscard]] Location locate(const memory::PhysicalFrame &frame) const
    {
        if (!pool_ || !valid_frame(frame)) return {};
        for (usize index = 0; index < files_.size(); ++index)
            if (files_[index].state == State::live)
                for (usize page = 0; page < files_[index].pages; ++page)
                    if (same_frame(frame, files_[index].frames[page])) return {index, page};
        return {};
    }
    Status retire(File &file)
    {
        if (file.description || file.aliases) return Status::busy("shared file still has an owner");
        file.state = State::retiring;
        Status result{};
        for (usize index = 0; index < file.pages; ++index) if (file.frames[index].owner)
        {
#if defined(OK_ENABLE_TEST_POINTS)
            const auto released = refuse_final_release_
                ? Status::fault("injected shared file final release refusal")
                : pool_->release_reference(file.frames[index]);
#else
            const auto released = pool_->release_reference(file.frames[index]);
#endif
            if (released) file.frames[index] = {};
            else
            {
                if (result) result = released;
                if (first_cleanup_failure_) first_cleanup_failure_ = released;
            }
        }
        if (result) file = {};
        return result;
    }
    Status release_staged(const memory::PhysicalFrame &frame)
    {
#if defined(OK_ENABLE_TEST_POINTS)
        if (refuse_staged_release_) return Status::fault("injected shared file staged release refusal");
#endif
        return pool_->release_reference(frame);
    }
    NativePagePool *pool_{};
    std::array<File, capacity> files_{};
    std::array<u64, capacity> generations_{};
    std::array<Cell, capacity * max_pages> ledger_{};
    u64 size_reservations_{};
    fs::IdentitySequence<> inode_sequence_{};
    Status first_cleanup_failure_{};
#if defined(OK_ENABLE_TEST_POINTS)
    usize refuse_allocation_after_{~usize{0}};
    bool refuse_staged_release_{}, refuse_final_release_{};
#endif
};
} // namespace ok::user::native
