#pragma once
#include "ok/core/fixed.hpp"
#include "ok/fs/attributes.hpp"
#include "ok/user/native_page_pool.hpp"
#include "ok/user/native_shared_file_handle.hpp"
#include <new>
#include <utility>

namespace ok::user::native
{
// Serialized boot-CPU provider. File pages and each alias have distinct pool
// references; allocator generations remain authoritative throughout retirement.
// The file table, each file's page index and the retirement ledger are
// kernel-object runs of the page pool: they grow on demand and are released
// once empty. The 16-bit handle slot bounds the file count; the pool's
// quotas bound file sizes.
class SharedMemoryFiles final : private NonCopyable
{
  public:
    static constexpr usize max_files = usize{1} << 16;
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
            if (auto *file = owner_->reserved(handle_))
            {
                *file = {};
                owner_->trim_files();
            }
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
        usize index = 0;
        while (index < file_capacity_ && files_[index].state != State::free) ++index;
        if (index == file_capacity_)
        {
            if (file_capacity_ >= max_files) return Status::system_file_limit("shared file object table is full");
            if (auto status = grow(files_, file_capacity_, file_pages_, file_capacity_ + 1, max_files); !status)
                return status;
        }
        if (next_generation_ == ~u64{0}) return Status::overflow("shared file generation sequence exhausted");
        auto &file = files_[index];
        if (auto status = file.name.assign(name); !status)
        {
            file = {};
            trim_files();
            return status;
        }
        auto inode = inode_sequence_.next();
        if (!inode)
        {
            file = {};
            trim_files();
            return inode.status();
        }
        file.state = State::reserved;
        file.uid = uid; file.gid = gid; file.inode = inode.value();
        file.generation = next_generation_++;
        const SharedFileHandle handle{static_cast<u64>(reinterpret_cast<uptr>(this)), file.generation,
            static_cast<u16>(index)};
        return Reservation{*this, handle};
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
        if (location.file == no_file)
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
        if (!size || size % page_size)
            return Status::unsupported("shared file size must be complete pages");
        const usize count = size / page_size;
        // A file can never hold more pages than the pool lets users own.
        if (count > pool_->user_capacity_count())
            return Status::no_memory("shared file size exceeds the user page quota");
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
        return locate(frame).file != no_file;
    }
    Status retain_alias(const memory::PhysicalFrame &frame)
    {
        const auto location = locate(frame);
        if (location.file == no_file) return Status::bad_descriptor("shared alias has no canonical file owner");
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
        if (location.file == no_file) return Status::bad_descriptor("shared alias has no canonical file owner");
        auto &file = files_[location.file];
        if (!file.aliases || !file.page_aliases[location.page])
            return Status::bad_descriptor("shared file page has no alias reference");
        if (auto status = pool_->release_reference(frame); !status) return status;
        --file.aliases;
        --file.page_aliases[location.page];
        consumed = true;
        if (file.aliases || file.description) return {};
        const auto retired = retire(file);
        trim_files();
        return retired;
    }
    Status release_description(SharedFileHandle handle)
    {
        auto *file = live(handle);
        if (!file || !file->description) return Status::bad_descriptor("shared file provider hold is stale");
        file->description = false;
        if (file->aliases) return {};
        const auto retired = retire(*file);
        trim_files();
        return retired;
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
        for (usize index = 0; index < cell_capacity_; ++index)
            if (auto &cell = cells_[index]; cell.state == LedgerState::retained)
            {
                const auto status = pool_->release_reference(cell.frame);
                if (status) cell = {};
                else if (result) result = status;
            }
        for (usize index = 0; index < file_capacity_; ++index)
            if (files_[index].state == State::retiring)
                if (auto status = retire(files_[index]); !status && result) result = status;
        trim_cells();
        trim_files();
        return result;
    }
    [[nodiscard]] LedgerInfo ledger() const
    {
        LedgerInfo result{};
        result.first_cleanup_failure = first_cleanup_failure_;
        for (usize index = 0; index < file_capacity_; ++index)
        {
            const auto &file = files_[index];
            result.files += file.state != State::free;
            result.retiring_files += file.state == State::retiring;
            result.aliases += file.aliases;
        }
        for (usize index = 0; index < cell_capacity_; ++index)
        {
            result.retained_frames += cells_[index].state == LedgerState::retained;
            result.reserved_cells += cells_[index].state == LedgerState::reserved;
        }
        return result;
    }
    [[nodiscard]] usize ledger_capacity() const { return cell_capacity_; }
    [[nodiscard]] const memory::PhysicalFrame *retained_frame(usize index) const
    {
        return index < cell_capacity_ && cells_[index].state == LedgerState::retained ? &cells_[index].frame : nullptr;
    }

  private:
#if defined(OK_ENABLE_TEST_POINTS)
    friend struct SharedMemoryFilesFaultProbe;
#endif
    enum class State : u8 { free, reserved, live, retiring };
    enum class LedgerState : u8 { free, reserved, retained };
    static constexpr usize no_file = ~usize{0};
    struct File
    {
        State state{};
        bool description{};
        usize size{}, pages{}, aliases{};
        u32 seals{};
        u32 uid{}, gid{};
        u64 inode{}, generation{};
        FixedString<250> name{};
        // Page index run: `pages` canonical frame tokens, then their alias counts.
        memory::PhysicalFrame *frames{};
        u16 *page_aliases{};
        usize index_pages{};
    };
    struct Cell
    {
        LedgerState state{};
        u64 reservation{};
        memory::PhysicalFrame frame{};
        Status failure{};
    };
    // Frames staged for one size initialization live in the future page index;
    // its reserved ledger cells take any frame whose rollback release fails.
    class SizeReservation final
    {
      public:
        SizeReservation(const SizeReservation &) = delete;
        SizeReservation &operator=(const SizeReservation &) = delete;
        SizeReservation &operator=(SizeReservation &&) = delete;
        SizeReservation(SizeReservation &&other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), frames_(other.frames_), pages_(other.pages_),
              count_(other.count_), staged_(other.staged_), identity_(other.identity_) {}
        ~SizeReservation()
        {
            if (!owner_) return;
            for (usize index = 0; index < staged_; ++index)
                if (frames_[index].owner)
                    if (auto released = owner_->release_staged(frames_[index]); !released)
                        owner_->retain_staged(identity_, frames_[index], released);
            owner_->finish_size_reservation(identity_);
            static_cast<void>(owner_->retire_run(reinterpret_cast<uptr>(frames_), pages_));
            owner_->trim_cells();
        }
        void stage(usize index, memory::PhysicalFrame frame)
        {
            frames_[index] = frame;
            staged_ = index + 1;
        }
        void publish(File &file, usize size)
        {
            file.frames = frames_;
            file.page_aliases = reinterpret_cast<u16 *>(frames_ + count_);
            file.index_pages = pages_;
            file.pages = count_;
            file.size = size;
            owner_->finish_size_reservation(identity_);
            owner_->trim_cells();
            owner_ = nullptr;
        }
      private:
        friend class SharedMemoryFiles;
        SizeReservation(SharedMemoryFiles &owner, memory::PhysicalFrame *frames, usize pages, usize count, u64 identity)
            : owner_(&owner), frames_(frames), pages_(pages), count_(count), identity_(identity) {}
        SharedMemoryFiles *owner_{};
        memory::PhysicalFrame *frames_{};
        usize pages_{}, count_{}, staged_{};
        u64 identity_{};
    };
    [[nodiscard]] Result<SizeReservation> reserve_size(usize count)
    {
        usize free{};
        for (usize index = 0; index < cell_capacity_; ++index) free += cells_[index].state == LedgerState::free;
        if (free < count)
            if (auto status = grow(cells_, cell_capacity_, cell_pages_, cell_capacity_ - free + count, ~usize{0}); !status)
                return status.code() == StatusCode::no_memory ? Status::no_memory("shared file retirement ledger is full")
                                                              : status;
        if (size_reservations_ == ~u64{0}) return Status::overflow("shared file size reservation sequence exhausted");
        const usize pages = (count * (sizeof(memory::PhysicalFrame) + sizeof(u16)) + page_size - 1) / page_size;
        auto index = pool_->allocate_object_run(pages);
        if (!index)
        {
            trim_cells();
            return index.status();
        }
        const auto identity = ++size_reservations_;
        usize held{};
        for (usize cell = 0; cell < cell_capacity_ && held < count; ++cell)
            if (cells_[cell].state == LedgerState::free)
            {
                cells_[cell].state = LedgerState::reserved;
                cells_[cell].reservation = identity;
                ++held;
            }
        return SizeReservation{*this, reinterpret_cast<memory::PhysicalFrame *>(index.value()), pages, count, identity};
    }
    void retain_staged(u64 identity, const memory::PhysicalFrame &frame, Status failure)
    {
        for (usize index = 0; index < cell_capacity_; ++index)
            if (auto &cell = cells_[index]; cell.state == LedgerState::reserved && cell.reservation == identity)
            {
                cell.state = LedgerState::retained;
                cell.frame = frame;
                cell.failure = failure;
                break;
            }
        if (first_cleanup_failure_) first_cleanup_failure_ = failure;
    }
    void finish_size_reservation(u64 identity)
    {
        for (usize index = 0; index < cell_capacity_; ++index)
            if (cells_[index].state == LedgerState::reserved && cells_[index].reservation == identity) cells_[index] = {};
    }
    // Doubles a table of `T` into a fresh run, keeping every entry.
    template <typename T> Status grow(T *&items, usize &capacity, usize &pages, usize minimum, usize limit)
    {
        usize wanted = capacity * 2 > minimum ? capacity * 2 : minimum;
        if (wanted > limit) wanted = limit;
        const usize grown_pages = (wanted * sizeof(T) + page_size - 1) / page_size;
        auto run = pool_->allocate_object_run(grown_pages);
        if (!run) return run.status();
        auto *grown = reinterpret_cast<T *>(run.value());
        usize grown_capacity = grown_pages * page_size / sizeof(T);
        if (grown_capacity > limit) grown_capacity = limit;
        for (usize index = 0; index < grown_capacity; ++index)
            new (&grown[index]) T{index < capacity ? items[index] : T{}};
        const auto previous = reinterpret_cast<uptr>(items);
        const auto previous_pages = std::exchange(pages, grown_pages);
        items = grown;
        capacity = grown_capacity;
        return previous_pages ? retire_run(previous, previous_pages) : Status{};
    }
    // An empty table goes back to the pool; generations outlive it.
    void trim_files()
    {
        for (usize index = 0; index < file_capacity_; ++index)
            if (files_[index].state != State::free) return;
        if (file_pages_) static_cast<void>(retire_run(reinterpret_cast<uptr>(files_), file_pages_));
        files_ = nullptr;
        file_capacity_ = file_pages_ = 0;
    }
    void trim_cells()
    {
        for (usize index = 0; index < cell_capacity_; ++index)
            if (cells_[index].state != LedgerState::free) return;
        if (cell_pages_) static_cast<void>(retire_run(reinterpret_cast<uptr>(cells_), cell_pages_));
        cells_ = nullptr;
        cell_capacity_ = cell_pages_ = 0;
    }
    Status retire_run(uptr address, usize pages)
    {
        if (auto status = pool_->release_object_run(address, pages); !status)
        {
            static_cast<void>(pool_->quarantine_object_run(address, pages));
            if (first_cleanup_failure_) first_cleanup_failure_ = status;
            return status;
        }
        return {};
    }
    struct Location { usize file{no_file}, page{}; };
    [[nodiscard]] bool matches(SharedFileHandle handle) const
    {
        return handle.owner == static_cast<u64>(reinterpret_cast<uptr>(this)) && handle.generation &&
            handle.slot < file_capacity_ && files_[handle.slot].generation == handle.generation;
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
        for (usize index = 0; index < file_capacity_; ++index)
            if (files_[index].state == State::live)
                for (usize page = 0; page < files_[index].pages; ++page)
                    if (same_frame(frame, files_[index].frames[page])) return {index, page};
        return {};
    }
    // Leaves the entry for the caller to trim; trimming may move no entry but
    // releases an empty table.
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
        if (!result) return result;
        if (file.index_pages)
            if (auto status = retire_run(reinterpret_cast<uptr>(file.frames), file.index_pages); !status) result = status;
        file = {};
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
    File *files_{};
    usize file_capacity_{}, file_pages_{};
    Cell *cells_{};
    usize cell_capacity_{}, cell_pages_{};
    u64 next_generation_{1};
    u64 size_reservations_{};
    fs::IdentitySequence<> inode_sequence_{};
    Status first_cleanup_failure_{};
#if defined(OK_ENABLE_TEST_POINTS)
    usize refuse_allocation_after_{~usize{0}};
    bool refuse_staged_release_{}, refuse_final_release_{};
#endif
};
} // namespace ok::user::native
