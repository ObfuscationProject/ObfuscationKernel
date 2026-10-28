// ELF admission, the initial stack (argv/envp/auxv), image installation and exec.
#include "private.hpp"

namespace ok::user::native
{
inline namespace runtime_detail
{
#ifdef OK_PRIVATE_PPC32_SIGNAL
namespace
{
// Pinned PPC musl debug files retain section tables after their debug data.
// This bounded private prefix is metadata storage, not executable image memory.
std::array<std::byte, 512 * 1024> elf_buffer{};
} // namespace
#else
namespace
{
std::array<std::byte, 128 * 1024> elf_buffer{};
} // namespace
#endif
namespace
{
// ELF admission reads metadata from a file prefix. When an architecture check
// needs more than the static buffer (a section table after debug data, an ABI
// record after the text), the prefix it asks for is read into a kernel object
// run, released when the load ends.
class ElfPrefix final : private NonCopyable
{
  public:
    static constexpr usize max_bytes = 16 * 1024 * 1024;
    ElfPrefix() = default;
    ~ElfPrefix() { if (run_) static_cast<void>(page_pool.release_object_run(run_, pages_)); }
    [[nodiscard]] std::byte *data() const { return run_ ? reinterpret_cast<std::byte *>(run_) : elf_buffer.data(); }
    [[nodiscard]] usize size() const { return size_; }
    [[nodiscard]] std::span<const std::byte> bytes() const { return {data(), size_}; }
    [[nodiscard]] Status read(fs::Node &node, usize size)
    {
        if (size > elf_buffer.size())
        {
            if (size > max_bytes) return Status::no_memory("ELF metadata prefix exceeds the admission bound");
            if (auto status = activate_pool(); !status) return status;
            const usize pages = (size + hw::page_size - 1) / hw::page_size;
            auto run = page_pool.allocate_object_run(pages);
            if (!run) return run.status();
            if (run_) static_cast<void>(page_pool.release_object_run(run_, pages_));
            run_ = run.value();
            pages_ = pages;
        }
        for (usize offset = 0; offset < size;)
        {
            auto bytes = node.read(offset, size - offset);
            if (!bytes)
                return bytes.status();
            if (bytes.value().size == 0 || bytes.value().size > size - offset)
                return Status::invalid_argument("truncated executable");
            copy_bytes(data() + offset, bytes.value().data.data(), bytes.value().size);
            offset += bytes.value().size;
        }
        size_ = size;
        return Status::success();
    }

  private:
    uptr run_{};
    usize pages_{}, size_{};
};
// The prefix the configured architecture's admission still needs, if more.
[[maybe_unused]] usize admission_prefix(std::span<const std::byte> bytes, usize file_bytes)
{
    usize required = 0;
    [[maybe_unused]] auto need = [&](usize bytes_needed) { if (bytes_needed > required) required = bytes_needed; };
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
    need(::ok::user::mips64_n64_elf_admission::admit(bytes, file_bytes).required_prefix_bytes);
#endif
#ifdef OK_PRIVATE_MIPS_O32_SIGNAL
    need(::ok::user::mips_o32_elf_admission::admit(bytes, file_bytes).required_prefix_bytes);
#endif
#ifdef OK_PRIVATE_PPC32_SIGNAL
    if (bytes.size() > 7)
        need(ppc32_elf_admission::admit(bytes, file_bytes,
            std::to_integer<u8>(bytes[7]) == 3 ? ppc32_elf_admission::Personality::linux_user
                                               : ppc32_elf_admission::Personality::native_os,
            ppc32_elf_admission::RuntimeProfile::e500v2_scalar_soft_float).required_prefix_bytes);
#endif
    return required;
}
} // namespace
// Exec argument and environment strings in one kernel object run, released
// with the object. As on Linux, each string with its NUL is at most
// MAX_ARG_STRLEN and the whole set at most ARG_MAX; beyond that execve fails
// with E2BIG.
class Arguments final : private NonCopyable
{
  public:
    static constexpr usize max_string_bytes = 32 * hw::page_size;
    static constexpr usize max_total_bytes = 64 * hw::page_size;
    static constexpr usize max_strings = 4096;

    Arguments() = default;
    Arguments(Arguments &&other) noexcept
        : run_(std::exchange(other.run_, 0)), pages_(std::exchange(other.pages_, 0)),
          argc_(std::exchange(other.argc_, 0)), envc_(std::exchange(other.envc_, 0)),
          used_(std::exchange(other.used_, 0)) {}
    Arguments &operator=(Arguments &&) = delete;
    ~Arguments() { if (run_) static_cast<void>(page_pool.release_object_run(run_, pages_)); }

    [[nodiscard]] Status push_argv(std::string_view text) { return insert(text, false, argc_); }
    [[nodiscard]] Status push_envp(std::string_view text) { return insert(text, true, envc_); }
    // A #! script runs as its interpreter with the script path before its own arguments.
    [[nodiscard]] Status prepend_argv(std::string_view text) { return insert(text, false, 0); }
    [[nodiscard]] Status replace_argv(usize index, std::string_view text)
    {
        if (index >= argc_) return Status::invalid_argument("exec argument index");
        if (auto status = insert(text, false, index); !status) return status;
        auto *list = entries(false);
        for (usize i = index + 1; i + 1 < argc_; ++i) list[i] = list[i + 1];
        --argc_;
        return Status::success();
    }
    [[nodiscard]] usize argc() const { return argc_; }
    [[nodiscard]] usize envc() const { return envc_; }
    [[nodiscard]] std::string_view argv(usize index) const { return view(entries(false)[index]); }
    [[nodiscard]] std::string_view envp(usize index) const { return view(entries(true)[index]); }
    // String bytes including each NUL, as they are copied to the new stack.
    [[nodiscard]] usize string_bytes() const
    {
        usize bytes = 0;
        for (usize i = 0; i < argc_; ++i) bytes += entries(false)[i].size + 1;
        for (usize i = 0; i < envc_; ++i) bytes += entries(true)[i].size + 1;
        return bytes;
    }

  private:
    struct Entry { u32 offset{}, size{}; };
    static constexpr usize index_bytes = max_strings * sizeof(Entry);
    [[nodiscard]] Entry *entries(bool environment) const
    { return reinterpret_cast<Entry *>(run_) + (environment ? max_strings / 2 : 0); }
    [[nodiscard]] std::string_view view(Entry entry) const
    { return {reinterpret_cast<const char *>(run_ + index_bytes + entry.offset), entry.size}; }
    [[nodiscard]] Status insert(std::string_view text, bool environment, usize at)
    {
        auto bytes = reserve(text.size(), environment, at);
        if (!bytes) return bytes.status();
        for (usize i = 0; i < text.size(); ++i) bytes.value()[i] = text[i];
        return Status::success();
    }
    // A new entry of `length` bytes plus its NUL; the caller fills the bytes.
    [[nodiscard]] Result<char *> reserve(usize length, bool environment, usize at)
    {
        auto &count = environment ? envc_ : argc_;
        if (length + 1 > max_string_bytes || count == max_strings / 2 || used_ + length + 1 > max_total_bytes)
            return Status::argument_list_too_long("exec arguments exceed ARG_MAX");
        if (auto status = grow(used_ + length + 1); !status) return status;
        auto *bytes = reinterpret_cast<char *>(run_ + index_bytes + used_);
        bytes[length] = 0;
        auto *list = entries(environment);
        for (usize i = count; i > at; --i) list[i] = list[i - 1];
        list[at] = {static_cast<u32>(used_), static_cast<u32>(length)};
        ++count;
        used_ += length + 1;
        return bytes;
    }
    friend Status argument_from_user(const hw::AddressSpace &, uptr, Arguments &, bool);
    [[nodiscard]] Status grow(usize bytes)
    {
        const usize needed = (index_bytes + bytes + hw::page_size - 1) / hw::page_size;
        if (needed <= pages_) return Status::success();
        if (auto status = activate_pool(); !status) return status;
        const usize pages = needed > 2 * pages_ ? needed : 2 * pages_;
        auto run = page_pool.allocate_object_run(pages);
        if (!run) return run.status();
        if (run_)
        {
            copy_bytes(reinterpret_cast<std::byte *>(run.value()), reinterpret_cast<const std::byte *>(run_),
                       index_bytes + used_);
            static_cast<void>(page_pool.release_object_run(run_, pages_));
        }
        run_ = run.value();
        pages_ = pages;
        return Status::success();
    }
    uptr run_{};
    usize pages_{}, argc_{}, envc_{}, used_{};
};
// A local loaded image must reserve failure retention before it acquires any
// pages. Publishing its complete directory into this cell is infallible; a
// failed backend or frame retirement cannot leave a local metadata destructor
// holding the last record of a live translation or exact data-page owner.
class ImageRetirementReservation final : private NonCopyable
{
  public:
    ImageRetirementReservation(const ImageRetirementReservation &) = delete;
    ImageRetirementReservation &operator=(const ImageRetirementReservation &) = delete;
    ImageRetirementReservation &operator=(ImageRetirementReservation &&) = delete;
    ImageRetirementReservation(ImageRetirementReservation &&other) noexcept
        : saved_(std::exchange(other.saved_, nullptr)) {}
    ~ImageRetirementReservation() { cancel(); }
    [[nodiscard]] static Result<ImageRetirementReservation> reserve()
    {
        for (auto &saved : quarantined_spaces)
            if (!saved.used && !saved.reserved)
            {
                saved.reserved = true;
                return ImageRetirementReservation{saved};
            }
        return Status::no_memory("native image retirement bookkeeping is full");
    }
    [[nodiscard]] bool active() const { return saved_ != nullptr; }
    void cancel()
    {
        if (saved_) saved_->reserved = false;
        saved_ = nullptr;
    }
    Status retire(hw::AddressSpace &space, Pages &pages) { return transfer(space, pages, true); }
    Status retain(hw::AddressSpace &space, Pages &pages) { return transfer(space, pages, false); }
  private:
    explicit ImageRetirementReservation(QuarantinedSpace &saved) : saved_(&saved) {}
    Status transfer(hw::AddressSpace &space, Pages &pages, bool destroy_table)
    {
        if (!saved_ || !saved_->reserved || saved_->used)
        {
            // Continuing would let the caller destroy the only metadata for a
            // live image. Move-only, serialized reservation ownership excludes
            // this breach; fail stopped if the invariant is ever violated.
            runtime_failure = Status::fault("native image retirement lost its reservation");
            __builtin_trap();
        }
        auto &saved = *std::exchange(saved_, nullptr);
        saved.pages = std::move(pages);
        saved.space = std::exchange(space, {});
        saved.used = true;
        saved.reserved = false;
        saved.table_retired = saved.space.root == 0;
        saved.pin_complete = saved.table_retired;
        saved.pinned_count = 0;
        saved.reset(QuarantinedSpace::Ownership::pending);
        for (const auto &page : saved.pages)
            if (!owned_frame(saved.pages, page))
                return Status::fault("native image retirement lost an exact owner token");
        Status result{};
        if (!saved.table_retired && destroy_table)
        {
            result = hw::destroy_space(saved.space);
            saved.table_retired = result.ok();
            saved.pin_complete = saved.table_retired;
        }
        if (!saved.table_retired)
        {
            for (usize index = 0; index < saved.pages.size(); ++index)
            {
                const auto *frame = owned_frame(saved.pages, saved.pages[index]);
                if (!frame) return Status::fault("native image pin lost exact owner token");
                if (auto status = page_pool.pin(*frame); !status) return status;
                saved.pin_held(index) = true;
                ++saved.pinned_count;
            }
            saved.pin_complete = true;
            if (!destroy_table) return Status::success();
        }
        const auto discharged = discharge_quarantine(saved);
        if (!saved.table_retired) return !result ? result : discharged;
        const auto retired = finish_quarantine(saved, false);
        if (!discharged) return discharged;
        return retired;
    }
    QuarantinedSpace *saved_{};
};
struct Image
{
    explicit Image(ImageRetirementReservation owner) : retirement(std::move(owner)) {}
    Image(const Image &) = delete;
    Image &operator=(const Image &) = delete;
    Image &operator=(Image &&) = delete;
    Image(Image &&other) noexcept
        : retirement(std::move(other.retirement)), pages(std::move(other.pages)),
          space(std::exchange(other.space, {}))
    {
#ifdef OK_PRIVATE_PPC32_SIGNAL
        signal_image = other.signal_image;
#endif
        personality = other.personality;
        frame = other.frame;
        memory = other.memory;
        program_break = other.program_break;
    }
    ~Image() { if (retirement.active()) static_cast<void>(retire()); }
    Status retire()
    {
        const auto status = retirement.retire(space, pages);
        if (!status) runtime_failure = status;
        return status;
    }
    Status retain_for_diagnosis()
    {
        const auto status = retirement.retain(space, pages);
        if (!status) runtime_failure = status;
        return status;
    }
    ImageRetirementReservation retirement;
#ifdef OK_PRIVATE_PPC32_SIGNAL
    Ppc32ImageAdmission signal_image{};
#endif
    Personality personality{Personality::native};
    Pages pages;
    hw::AddressSpace space;
    hw::UserFrame frame;
    sched::MemoryMap memory;
    uptr program_break{hw::user_begin};
};
namespace
{
Result<Image> load(Kernel &kernel, std::string_view path, const Arguments &args)
{
    auto resolved = kernel.posix().resolve_node(posix::at_FDCWD, path);
    if (!resolved)
        return resolved.status();
    auto *node = resolved.value().node();
    auto queried = node->query_metadata();
    if (!queried)
        return queried.status();
    const auto metadata = queried.value();
    if (metadata.type != fs::NodeType::regular || (metadata.mode & 0111u) == 0)
        return Status::denied("file is not executable");
    if (auto status = fs::require_access(metadata, {kernel.posix().euid(), kernel.posix().egid()}, fs::access_execute);
        !status.ok())
        return status;
    ElfPrefix prefix;
    if (auto status = prefix.read(*node, metadata.size < elf_buffer.size() ? metadata.size : elf_buffer.size());
        !status.ok())
        return status;
    // Each grown prefix can expose one further record (section names, then
    // attributes), so a few rounds cover every bounded admission.
    for (usize round = 0; round < 4; ++round)
    {
        const usize required = admission_prefix(prefix.bytes(), metadata.size);
        if (required <= prefix.size() || required > metadata.size) break;
        if (auto status = prefix.read(*node, required); !status.ok())
            return status;
    }
    const usize prefix_size = prefix.size();
    std::byte *const image_bytes = prefix.data();
    constexpr u8 native_encoding = __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__ ? 2 : 1;
    if (metadata.size < 4 || image_bytes[0] != std::byte{0x7f} || image_bytes[1] != std::byte{'E'} ||
        image_bytes[2] != std::byte{'L'} || image_bytes[3] != std::byte{'F'})
        return Status::exec_format("not an ELF executable");
    if (metadata.size < 16 || std::to_integer<u8>(image_bytes[4]) != (sizeof(uptr) == 8 ? 2 : 1) ||
        std::to_integer<u8>(image_bytes[5]) != native_encoding)
        return Status::invalid_argument("ELF class or byte order does not match the native ABI");
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
    // A required prefix beyond the explicit 128 KiB bound is unsupported. The
    // serialized file read above and ordinary loader still own mapping checks.
    if (!::ok::user::mips64_n64_elf_admission::admit({image_bytes, prefix_size}, metadata.size))
        return Status::unsupported("private n64 signal ELF profile is not admitted");
#endif
#ifdef OK_PRIVATE_MIPS_O32_SIGNAL
    if (!::ok::user::mips_o32_elf_admission::admit({image_bytes, prefix_size}, metadata.size).ok())
        return Status::unsupported("private MIPS signal ELF profile is not admitted");
#endif
    auto elf = sched::ElfLoader{}.load_prefix({image_bytes, prefix_size}, metadata.size, kernel.arch().architecture());
    if (!elf)
        return elf.status();
    auto retained = ImageRetirementReservation::reserve();
    if (!retained) return retained.status();
    Image image{std::move(retained.value())};
    lu::ExecMetadata linux_metadata{};
    bool dynamic_x86 = false;
    uptr interpreter_entry = 0;
#if defined(__x86_64__)
    constexpr std::string_view x86_interpreter_path = "/lib/ld-musl-x86_64.so.1";
    uptr entry_bias = 0;
#endif
    const auto osabi = std::to_integer<u8>(image_bytes[7]);
    if (osabi == 3)
    {
        constexpr u8 abi_version = LinuxExec::architecture == arch::Architecture::mips ||
                                   LinuxExec::architecture == arch::Architecture::mips64 ? 1 : 0;
        if (std::to_integer<u8>(image_bytes[8]) != abi_version)
            return Status::unsupported("Linux ELF ABI version differs from configured profile");
        const usize linux_prefix = prefix_size < LinuxExec::max_prefix_bytes ? prefix_size : LinuxExec::max_prefix_bytes;
        if (elf.value().static_executable)
        {
            auto inspected = LinuxExec::inspect({image_bytes, linux_prefix}, metadata.size);
            if (!inspected)
                return inspected.status();
            linux_metadata = inspected.value().metadata;
        }
        else
        {
#if defined(__x86_64__)
            auto inspected = lu::X86DynamicExec::inspect({image_bytes, linux_prefix}, metadata.size, false);
            if (!inspected)
                return inspected.status();
            const auto path_offset = inspected.value().interpreter_offset;
            const auto path_size = inspected.value().interpreter_size;
            if (path_size != x86_interpreter_path.size() + 1 ||
                path_offset > linux_prefix || path_size > linux_prefix - path_offset)
                return Status::unsupported("x86-64 dynamic ELF interpreter path differs from pinned musl loader");
            for (usize i = 0; i < x86_interpreter_path.size(); ++i)
                if (std::to_integer<u8>(image_bytes[path_offset + i]) != static_cast<u8>(x86_interpreter_path[i]))
                    return Status::unsupported("x86-64 dynamic ELF interpreter path differs from pinned musl loader");
            if (image_bytes[path_offset + x86_interpreter_path.size()] != std::byte{})
                return Status::invalid_argument("x86-64 dynamic ELF interpreter path is not terminated");
            dynamic_x86 = true;
            entry_bias = 0x42000000;
            linux_metadata = inspected.value().metadata;
            if (linux_metadata.program_headers > ~uptr{0} - entry_bias ||
                linux_metadata.entry > ~uptr{0} - entry_bias)
                return Status::overflow("x86-64 dynamic main metadata relocation wraps");
            linux_metadata.program_headers += entry_bias;
            linux_metadata.entry += entry_bias;
#else
            return Status::unsupported("dynamic Linux images require an architecture-qualified loader");
#endif
        }
        image.personality = Personality::linux;
    }
    else if (osabi != 0)
        return Status::unsupported("unsupported ELF syscall personality");
    else if (!elf.value().static_executable)
        return Status::unsupported("native dynamic ELF images are unsupported");
#ifdef OK_PRIVATE_PPC32_SIGNAL
    image.signal_image = Ppc32ImageAdmission::inspect(private_ppc_backend::capability(),
        {image_bytes, prefix_size}, metadata.size,
        image.personality == Personality::linux ? ppc32_elf_admission::Personality::linux_user :
            ppc32_elf_admission::Personality::native_os,
        ppc32_elf_admission::RuntimeProfile::e500v2_scalar_soft_float);
    if (!image.signal_image.valid(private_ppc_backend::capability()))
        return Status::unsupported("private PPC ELF profile is not admitted");
#endif
    auto fail = [&](Status status) -> Result<Image> {
        return status;
    };
    constexpr uptr stack_top = hw::user_end - 0x100000;
    // The initial stack holds the argument strings and vectors above 64 KiB of
    // ordinary stack.
    const usize argument_bytes = args.string_bytes() + (args.argc() + args.envc() + 3) * sizeof(uptr);
    const usize stack_size = 16 * hw::page_size + ((argument_bytes + hw::page_size - 1) & ~(hw::page_size - 1));
    const uptr main_bias = dynamic_x86 ? uptr{0x42000000} : uptr{0};
    auto map_loaded = [&](const sched::LoadedElf &loaded, fs::Node *source,
                          usize file_size, uptr bias, bool main_program) -> Status {
        bool entry_executable = false;
        const uptr load_limit = stack_top - stack_size - hw::page_size;
        if (loaded.entry > ~uptr{0} - bias)
            return Status::overflow("ELF entry relocation wraps");
        const uptr entry = loaded.entry + bias;
        for (const auto &segment : loaded.segments)
        {
            if (segment.memory_size == 0)
                continue;
            if (segment.virtual_address > ~uptr{0} - bias)
                return Status::overflow("ELF segment relocation wraps");
            const uptr address = segment.virtual_address + bias;
            if (address < hw::user_begin || address >= load_limit ||
                segment.memory_size > load_limit - address ||
                segment.file_offset > file_size || segment.file_size > file_size - segment.file_offset)
                return Status::invalid_argument("ELF segment outside user window");
            u8 flags = hw::page_read;
            if (segment.permissions & memory::page_write)
                flags |= hw::page_write;
            if (segment.permissions & memory::page_execute)
                flags |= hw::page_execute;
            if ((flags & (hw::page_write | hw::page_execute)) == (hw::page_write | hw::page_execute))
                return Status::denied("writable executable pages are forbidden");
            if ((flags & hw::page_execute) && entry >= address && entry - address < segment.memory_size)
                entry_executable = true;
            const uptr begin = address & ~(hw::page_size - 1);
            const uptr end = (address + segment.memory_size + hw::page_size - 1) & ~(hw::page_size - 1);
            if (main_program && end > image.program_break)
                image.program_break = end;
            if (auto status = image.memory.add_area({begin, end - begin, segment.permissions}); !status.ok())
                return status;
            for (uptr page = begin; page < end; page += hw::page_size)
                if (auto status = allocate(image.pages, page, flags); !status.ok())
                    return status;
            // Source identity stays retained until all unpublished pages are populated.
            usize copied = 0;
            while (copied < segment.file_size)
            {
                auto bytes = source->read(segment.file_offset + copied, segment.file_size - copied);
                if (!bytes)
                    return bytes.status();
                if (bytes.value().size == 0 || bytes.value().size > segment.file_size - copied)
                    return Status::invalid_argument("truncated ELF load segment");
                if (auto status = store(image.pages, address + copied, bytes.value().data.data(),
                                        bytes.value().size); !status.ok())
                    return status;
                copied += bytes.value().size;
            }
        }
        return entry_executable ? Status::success() : Status::invalid_argument("ELF entry is not executable");
    };
    if (auto status = map_loaded(elf.value(), node, metadata.size, main_bias, true); !status.ok())
        return fail(status);
#if defined(__x86_64__)
    if (dynamic_x86)
    {
        auto interpreter = kernel.posix().resolve_node(posix::at_FDCWD, x86_interpreter_path);
        if (!interpreter)
            return fail(interpreter.status());
        auto *interpreter_node = interpreter.value().node();
        auto interpreter_query = interpreter_node->query_metadata();
        if (!interpreter_query)
            return fail(interpreter_query.status());
        const auto interpreter_metadata = interpreter_query.value();
        if (interpreter_metadata.type != fs::NodeType::regular || (interpreter_metadata.mode & 0111u) == 0)
            return fail(Status::denied("musl interpreter is not executable"));
        if (auto status = fs::require_access(interpreter_metadata,
                {kernel.posix().euid(), kernel.posix().egid()}, fs::access_execute); !status.ok())
            return fail(status);
        const usize interpreter_prefix = interpreter_metadata.size < lu::X86DynamicExec::max_prefix_bytes ?
            interpreter_metadata.size : lu::X86DynamicExec::max_prefix_bytes;
        usize interpreter_offset = 0;
        while (interpreter_offset < interpreter_prefix)
        {
            auto bytes = interpreter_node->read(interpreter_offset, interpreter_prefix - interpreter_offset);
            if (!bytes)
                return fail(bytes.status());
            if (bytes.value().size == 0 || bytes.value().size > interpreter_prefix - interpreter_offset)
                return fail(Status::invalid_argument("truncated musl interpreter prefix"));
            copy_bytes(elf_buffer.data() + interpreter_offset, bytes.value().data.data(), bytes.value().size);
            interpreter_offset += bytes.value().size;
        }
        auto inspected = lu::X86DynamicExec::inspect(
            {elf_buffer.data(), interpreter_prefix}, interpreter_metadata.size, true);
        if (!inspected)
            return fail(inspected.status());
        constexpr uptr interpreter_bias = 0x60000000;
        if (auto status = map_loaded(inspected.value().loaded, interpreter_node,
                                      interpreter_metadata.size, interpreter_bias, false); !status.ok())
            return fail(status);
        interpreter_entry = interpreter_bias + inspected.value().loaded.entry;
        linux_metadata.interpreter_base = interpreter_bias;
        if (auto status = interpreter.value().release(); !status.ok())
            return fail(status);
    }
#endif
    // Neither source identity is released until both unpublished images are complete.
    if (auto status = resolved.value().release(); !status.ok())
        return fail(status);
    for (uptr address = stack_top - stack_size; address < stack_top; address += hw::page_size)
        if (auto status = allocate(image.pages, address, hw::page_read | hw::page_write); !status.ok())
            return fail(status);
    static_cast<void>(image.memory.add_area(
        {stack_top - stack_size, stack_size, memory::page_user | memory::page_read | memory::page_write}));
    // Strings ascend from argv[0] to the last environment string, as on Linux.
    const uptr strings_begin = stack_top - args.string_bytes();
    uptr sp = strings_begin;
    {
        uptr cursor = strings_begin;
        auto strings = [&](usize count, bool environment) -> Status {
            for (usize i = 0; i < count; ++i)
            {
                const auto text = environment ? args.envp(i) : args.argv(i);
                const char terminator = 0;
                if (auto status = store(image.pages, cursor, text.data(), text.size()); !status.ok())
                    return status;
                if (auto status = store(image.pages, cursor + text.size(), &terminator, 1); !status.ok())
                    return status;
                cursor += text.size() + 1;
            }
            return Status::success();
        };
        if (auto status = strings(args.argc(), false); !status.ok())
            return fail(status);
        if (auto status = strings(args.envc(), true); !status.ok())
            return fail(status);
    }
    // argc, the argv and envp pointer vectors (each NULL-terminated) and the
    // auxiliary vector form one record that starts 16-byte aligned.
    std::array<u8, LinuxExec::max_auxv_bytes> auxv{};
    usize auxv_bytes = 0;
    if (image.personality == Personality::linux)
    {
        sp -= path.size() + 1;
        if (auto status = store(image.pages, sp, path.data(), path.size()); !status.ok())
            return fail(status);
        const char terminator = 0;
        if (auto status = store(image.pages, sp + path.size(), &terminator, 1); !status.ok())
            return fail(status);
        std::array<std::byte, 16> random{};
        auto acquired = kernel.posix().getrandom(random);
        if (!acquired)
            return fail(acquired.status());
        if (acquired.value() != random.size())
            return fail(Status::would_block("incomplete AT_RANDOM entropy"));
        sp -= random.size();
        if (auto status = store(image.pages, sp, random.data(), random.size()); !status.ok())
            return fail(status);
        const uptr random_address = sp;
        auto encoded = LinuxExec::encode_auxv(auxv, linux_metadata,
            {.page_size = hw::page_size, .uid = kernel.posix().uid(), .euid = kernel.posix().euid(),
             .gid = kernel.posix().gid(), .egid = kernel.posix().egid(),
             .secure = kernel.posix().uid() != kernel.posix().euid() || kernel.posix().gid() != kernel.posix().egid(),
             .execfn = sp + random.size(), .random = random_address});
        if (!encoded)
            return fail(encoded.status());
        auxv_bytes = encoded.value();
    }
    else
    {
        const std::array<uptr, 6> native_auxv{6, hw::page_size, 9, elf.value().entry, 0, 0}; // AT_PAGESZ, AT_ENTRY
        copy_bytes(auxv.data(), native_auxv.data(), sizeof(native_auxv));
        auxv_bytes = sizeof(native_auxv);
    }
    const usize vector_bytes = (1 + args.argc() + 1 + args.envc() + 1) * sizeof(uptr);
    sp = (sp - vector_bytes - auxv_bytes) & ~uptr(15);
    {
        uptr cursor = sp, string = strings_begin;
        auto word = [&](uptr value) -> Status {
            const auto status = store(image.pages, cursor, &value, sizeof(value));
            cursor += sizeof(value);
            return status;
        };
        if (auto status = word(args.argc()); !status.ok())
            return fail(status);
        for (usize i = 0; i < args.argc(); string += args.argv(i).size() + 1, ++i)
            if (auto status = word(string); !status.ok())
                return fail(status);
        if (auto status = word(0); !status.ok())
            return fail(status);
        for (usize i = 0; i < args.envc(); string += args.envp(i).size() + 1, ++i)
            if (auto status = word(string); !status.ok())
                return fail(status);
        if (auto status = word(0); !status.ok())
            return fail(status);
        if (auto status = store(image.pages, cursor, auxv.data(), auxv_bytes); !status.ok())
            return fail(status);
    }
#ifdef OK_PRIVATE_SIGNAL_HAS_TRAMPOLINE
    if (image.personality == Personality::linux)
    {
        // ELF admission excludes this region above the initial stack. Fill an
        // owned page before publishing RX PTEs; there is no user-writable alias.
        constexpr auto code = PrivateSignalCode::image();
        if (auto status = allocate(image.pages, private_signal_code_address, hw::page_read | hw::page_execute); !status)
            return fail(status);
        if (auto status = store(image.pages, private_signal_code_address, code.data(), code.size()); !status)
            return fail(status);
        if (auto status = image.memory.add_area({private_signal_code_address, hw::page_size,
                memory::page_user | memory::page_read | memory::page_execute}); !status)
            return fail(status);
        // RISC-V entry executes fence.i; LoongArch entry executes dbar/ibar
        // before this new page executes on the serialized boot CPU.
    }
#endif
    auto space = create_page_space(image.pages);
    if (!space)
        return fail(space.status());
    image.space = space.value();
    image.frame = hw::initial_frame(dynamic_x86 ? interpreter_entry : elf.value().entry, sp);
    return image;
}
} // namespace
// Copy one NUL-terminated user string, a page-bounded chunk at a time.
Status argument_from_user(const hw::AddressSpace &space, uptr address, Arguments &out, bool environment)
{
    std::array<char, 256> chunk{};
    usize length = 0;
    for (;;)
    {
        if (address + length < address) return Status::fault("invalid user string");
        const uptr at = address + length;
        const usize to_page = hw::page_size - (at & (hw::page_size - 1));
        const usize count = to_page < chunk.size() ? to_page : chunk.size();
        if (!hw::copy_from_user(space, chunk.data(), at, count)) return Status::fault("invalid user string");
        usize used = 0;
        while (used < count && chunk[used]) ++used;
        length += used;
        if (length + 1 > Arguments::max_string_bytes)
            return Status::argument_list_too_long("exec string exceeds MAX_ARG_STRLEN");
        if (used < count) break;
    }
    // Copy into the arena now that the length is known and bounded.
    auto bytes = out.reserve(length, environment, environment ? out.envc() : out.argc());
    if (!bytes) return bytes.status();
    if (!hw::copy_from_user(space, bytes.value(), address, length)) return Status::fault("invalid user string");
    return Status::success();
}
Status arguments_from_user(const hw::AddressSpace &space, uptr pointer, Arguments &out, bool environment)
{
    if (pointer == 0)
        return Status::success();
    for (usize i = 0;; ++i)
    {
        uptr value{};
        const uptr address = pointer + i * sizeof(uptr);
        if (address < pointer || !hw::copy_from_user(space, &value, address, sizeof(value)))
            return Status::fault("invalid argument vector");
        if (value == 0)
            return Status::success();
        if (auto status = argument_from_user(space, value, out, environment); !status.ok())
            return status;
    }
}
namespace
{
Status install(sched::ProcessControlBlock &process, Image &image, sched::SignalMask mask = {})
{
    // Native bridges currently execute only on the boot CPU.
    bool previous_quarantined = false;
    for (const auto &saved : quarantined_spaces)
        previous_quarantined |= saved.space.root && same_space(saved.space, process.resources().native_space);
    if (!process.resources().native_pages.empty() ||
        (process.resources().native_space.root && !previous_quarantined))
        return Status::busy("previous native image must retire before installation");
    auto &resources = process.resources();
    auto &thread = process.threads()[0];
    if (auto status = cancel_native_vfs_thread(thread); !status) return status;
    const auto previous_thread = thread;
    const auto previous_memory = resources.memory;
    const auto previous_space = resources.native_space;
    const auto previous_personality = resources.personality;
    const auto previous_break_base = resources.program_break_base;
    const auto previous_break = resources.program_break;
    const auto previous_break_limit = resources.program_break_limit;
    const auto previous_affinity = process.cpu_affinity_mask();
    bool origin_prepared = false;
    auto fail_prepare = [&](Status status) {
        if (origin_prepared) syscall_origins.retire(thread);
        vfs_continuations.revoke(thread);
        thread = previous_thread;
        resources.memory = previous_memory;
        resources.native_space = previous_space;
        resources.personality = previous_personality;
        resources.program_break_base = previous_break_base;
        resources.program_break = previous_break;
        resources.program_break_limit = previous_break_limit;
        process.set_cpu_affinity_mask(previous_affinity);
        return status;
    };
    process.set_cpu_affinity_mask(1);
    if (image.personality == Personality::linux)
    {
        uptr first = ~uptr{0}, last = 0;
        for (const auto &page : image.pages)
        {
            if (!owned_frame(image.pages, page))
                runtime_failure = Status::fault("Linux image lost frame ownership");
            if (page.physical_address < first) first = page.physical_address;
            if (page.physical_address > last) last = page.physical_address;
        }
        emit("\nOK_NATIVE_IMAGE personality=linux source=");
        emit(page_pool.source() == NativePagePool::Source::qualified_firmware ? "firmware" : "linker");
        emit(" pages="); emit_number(image.pages.size());
        emit(" physical_first="); emit_hex(first);
        emit(" physical_last="); emit_hex(last); emit(" owner=retained\n");
    }
    resources.personality = image.personality;
    resources.native_space = image.space;
    resources.program_break_base = image.program_break;
    resources.program_break = image.program_break;
    resources.program_break_limit = hw::user_end - 0x100000 - 17 * hw::page_size;
    thread.tls = {};
    thread.native_signal_mask = mask;
    thread.native_frame = image.frame;
    thread.native_active = true;
    thread.syscall_pending = false;
    thread.native_deadline.cancel();
    thread.wait_registration.cancel();
    process.memory_map() = image.memory;
    if (auto status = syscall_origins.prepare(thread); !status) return fail_prepare(status);
    origin_prepared = true;
    if (auto status = vfs_continuations.prepare(thread); !status) return fail_prepare(status);
#ifdef OK_PRIVATE_PPC32_SIGNAL
    if (image.personality == Personality::linux && !private_ppc_initial(process, thread, image.signal_image))
        return fail_prepare(Status::fault("private PPC initial image admission failed"));
#endif
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
    if (!private_n64_initial(process, thread))
        return fail_prepare(Status::fault("private n64 initial signal context publication failed"));
#endif
#ifdef OK_PRIVATE_MIPS_O32_SIGNAL
    if (!private_mips_initial(thread))
        return fail_prepare(Status::fault("private MIPS initial signal context publication failed"));
#endif
    // Every fallible origin and signal preparation has completed while the
    // staged image still owns records borrowed by its backend. Transfer only
    // now; block addresses do not change when the directory moves.
    resources.native_pages = std::move(image.pages);
    image.space = {};
    image.retirement.cancel();
    return Status::success();
}
} // namespace
template <typename Install>
Status retire_then_install(sched::ProcessResources &resources, Image &image,
                           ImageRetirementReservation &previous, Install &&install_image)
{
    const auto retired = previous.retire(resources.native_space, resources.native_pages);
    if (!retired)
    {
        static_cast<void>(image.retire());
        runtime_failure = retired;
        return retired;
    }
    const auto installed = install_image();
    if (!installed)
    {
        static_cast<void>(image.retire());
        runtime_failure = installed;
    }
    return installed;
}
class VfsImageRetirement final : private NonCopyable
{
  public:
    Status prepare(sched::SchedulerState &scheduler, sched::ProcessControlBlock &process)
    {
        if (staged_ || scheduler.find(process.pid()) != &process || process.threads().empty())
            return Status::denied("VFS image retirement has no canonical source image");
        staged_ = true;
        process_ = &process; scheduler_ = &scheduler; pid_ = process.pid();
        image_ = process.address_space_id(); space_ = process.resources().native_space;
        usize requests{};
        for (const auto &thread : process.threads())
        {
            auto handle = vfs_continuations.pending(pid_, thread.tid);
            const auto ticket = vfs_continuations.ticket(handle);
            if (handle.request)
            {
                ++requests;
                const bool active = thread.state == sched::ProcessState::blocked &&
                    thread.wait_reason == sched::ThreadWaitReason::vfs_operation && thread.wait_key == handle.request &&
                    thread.wait_registration.pending(ticket);
                const bool ready = !thread.wait_registration.active() && thread.wait_registration.ticket() == ticket &&
                    thread.wait_registration.completion() == sched::WaitCompletion::ready &&
                    thread.wait_reason == sched::ThreadWaitReason::none && thread.wait_key == 0 &&
                    (thread.state == sched::ProcessState::runnable || thread.state == sched::ProcessState::running);
                if (!thread.entry_stack.pool || scheduler.find_thread(pid_, thread.tid) != &thread || (!active && !ready))
                    return Status::fault("VFS image retirement lost its exact old scheduler ticket");
            }
            records_[count_++] = {thread.tid, thread.entry_stack, handle, ticket, thread.wait_registration.completion()};
        }
        if (vfs_continuations.pending_count(pid_) != requests)
            return Status::fault("VFS image retirement found a request without its canonical old thread");
        requests_ = requests;
        prepared_ = true;
        return {};
    }

    Status consume(sched::SchedulerState &scheduler, sched::ProcessControlBlock &process)
    {
        if (!prepared_ || consumed_ || scheduler_ != &scheduler || process_ != &process ||
            scheduler.find(pid_) != &process || process.pid() != pid_ || process.address_space_id() == image_ ||
            !same_space(process.resources().native_space, space_) || process.threads().size() != 1)
            return Status::fault("VFS image cancellation requires its committed scheduler image");
        if (vfs_continuations.pending_count(pid_) != requests_)
            return Status::fault("VFS image cancellation changed its staged request count");
        const auto &leader = process.threads()[0];
        const bool leader_lease = records_[0].lease.pool ? leader.entry_stack == records_[0].lease
            : !records_[0].handle.request && entry_stacks.validate_inactive(leader.entry_stack, {pid_, records_[0].tid}).ok();
        if (leader.tid != records_[0].tid || !leader_lease ||
            leader.state != sched::ProcessState::runnable || leader.wait_registration.active() ||
            leader.wait_reason != sched::ThreadWaitReason::none || leader.wait_key)
            return Status::fault("VFS image cancellation found a replacement live wait or lease");
        // Validate the entire plan before cancelling any request. Missing old
        // peers are valid only at this committed scheduler collapse boundary.
        for (usize i = 0; i < count_; ++i)
        {
            const auto &old = records_[i];
            if (i && scheduler.find_thread(pid_, old.tid))
                return Status::fault("VFS image cancellation retained an unexpected old peer");
            if (old.lease.pool)
                if (auto status = entry_stacks.validate_inactive(old.lease, {pid_, old.tid}); !status) return status;
            if (vfs_continuations.pending(pid_, old.tid) != old.handle ||
                vfs_continuations.ticket(old.handle) != old.ticket)
                return Status::fault("VFS image cancellation changed its pending request or ticket");
            if (!i && old.handle.request)
            {
                const auto expected = old.completion == sched::WaitCompletion::ready
                    ? sched::WaitCompletion::ready : sched::WaitCompletion::cancelled;
                if (leader.wait_registration.ticket() != old.ticket || leader.wait_registration.completion() != expected)
                    return Status::fault("VFS image cancellation lost its exact committed leader ticket");
            }
        }
        for (usize i = 0; i < count_; ++i)
            if (records_[i].handle.request)
                if (auto status = vfs_continuations.cancel(scheduler, records_[i].handle); !status) return status;
        consumed_ = true;
        return {};
    }

  private:
    using Continuation = VfsStatContinuation<lu::configured_native_profile()>;
    struct Record
    {
        sched::ThreadId tid{};
        memory::KernelEntryStackHandle lease{};
        Continuation::Handle handle{};
        sched::WaitTicket ticket{};
        sched::WaitCompletion completion{};
    };
    static bool same_space(hw::AddressSpace a, hw::AddressSpace b)
    { return a.root == b.root && a.slot == b.slot && a.generation == b.generation; }
    std::array<Record, sched::max_threads_per_process> records_{};
    sched::SchedulerState *scheduler_{};
    sched::ProcessControlBlock *process_{};
    sched::ProcessId pid_{};
    u64 image_{};
    hw::AddressSpace space_{};
    usize count_{}, requests_{};
    bool staged_{}, prepared_{}, consumed_{};
};
struct ExecRetirement
{
    struct Thread { sched::ThreadId tid{}; sched::ThreadLocalState tls{}; };
    StaticVector<Thread, sched::max_threads_per_process> threads;
    sched::ThreadId caller{};
    ExecRetirement(const sched::ProcessControlBlock &process, sched::ThreadId invoking) : caller(invoking)
    {
        for (const auto &thread : process.threads())
            if (thread.state != sched::ProcessState::exited)
                static_cast<void>(threads.push_back({thread.tid, thread.tls}));
    }
    void consume(Kernel &kernel, sched::ProcessControlBlock &process)
    {
        syscall_origins.revoke_process(process.pid());
        cancel_pipe_read(kernel, process.pid());
        cancel_pipe_write(kernel, process.pid());
        cancel_pty_waits(kernel, process.pid());
        cancel_child_wait(kernel, process.pid());
        // Scheduler collapse is committed, but the process still owns its old
        // VM. No user can observe this boundary until all retirement is done.
        for (auto &thread : threads)
            if (thread.tid != caller)
            {
                static_cast<void>(retire_robust(kernel, process, thread.tls, thread.tid));
                clear_child_tid(kernel, process, thread.tls, true);
            }
        for (auto &thread : threads)
            if (thread.tid == caller)
            {
                // Linux de_thread transfers the process TID before caller
                // futex_exec_release. The old TID must not be used here.
                static_cast<void>(retire_robust(kernel, process, thread.tls, process.pid()));
                clear_child_tid(kernel, process, thread.tls, false);
            }
        threads.clear();
    }
};
namespace
{
// Linux binfmt_script: an executable whose first line is "#!interpreter [arg]"
// runs as `interpreter [arg] path argv[1..]`. The line must fit 256 bytes.
struct Script
{
    FixedString<256> interpreter, argument;
    bool has_argument{};
};
Result<bool> script_interpreter(Kernel &kernel, std::string_view path, Script &out)
{
    auto resolved = kernel.posix().resolve_node(posix::at_FDCWD, path);
    if (!resolved)
        return resolved.status();
    auto *node = resolved.value().node();
    auto queried = node->query_metadata();
    Result<bool> result{false};
    if (!queried)
        result = queried.status();
    else if (queried.value().type == fs::NodeType::regular && queried.value().size >= 2)
    {
        auto bytes = node->read(0, 256);
        if (!bytes)
            result = bytes.status();
        else if (bytes.value().size >= 2 && bytes.value().data[0] == std::byte{'#'} &&
                 bytes.value().data[1] == std::byte{'!'})
        {
            // As for ELF images, even root needs one execute bit.
            if ((queried.value().mode & 0111u) == 0)
                result = Status::denied("script is not executable");
            else if (auto status = fs::require_access(queried.value(), {kernel.posix().euid(), kernel.posix().egid()},
                                                      fs::access_execute); !status.ok())
                result = status;
            else
            {
                const auto *line = reinterpret_cast<const char *>(bytes.value().data.data());
                usize end = 2;
                while (end < bytes.value().size && line[end] != '\n') ++end;
                if (end == bytes.value().size && bytes.value().size == 256)
                    result = Status::exec_format("script interpreter line exceeds 256 bytes");
                else
                {
                    auto blank = [](char c) { return c == ' ' || c == '\t'; };
                    usize at = 2;
                    while (at < end && blank(line[at])) ++at;
                    const usize name = at;
                    while (at < end && !blank(line[at]) && line[at]) ++at;
                    usize tail = end;
                    while (tail > at && (blank(line[tail - 1]) || !line[tail - 1])) --tail;
                    usize argument = at;
                    while (argument < tail && blank(line[argument])) ++argument;
                    if (at == name)
                        result = Status::exec_format("script names no interpreter");
                    else if (auto status = out.interpreter.assign({line + name, at - name}); !status.ok())
                        result = status;
                    else if (auto status = out.argument.assign({line + argument, tail - argument}); !status.ok())
                        result = status;
                    else
                    {
                        out.has_argument = argument < tail;
                        result = true;
                    }
                }
            }
        }
    }
    if (auto status = resolved.value().release(); !status.ok())
        return status;
    return result;
}
// Rewrites `path` and `arguments` through at most four nested interpreters.
Status resolve_scripts(Kernel &kernel, FixedString<256> &path, Arguments &arguments)
{
    for (usize depth = 0;; ++depth)
    {
        Script script;
        auto found = script_interpreter(kernel, path.view(), script);
        if (!found)
            return found.status();
        if (!found.value())
            return Status::success();
        if (depth == 4)
            return Status::too_many_symlinks("script interpreters nest more than four levels");
        if (arguments.argc())
        {
            if (auto status = arguments.replace_argv(0, path.view()); !status.ok())
                return status;
        }
        else if (auto status = arguments.push_argv(path.view()); !status.ok())
            return status;
        if (script.has_argument)
            if (auto status = arguments.prepend_argv(script.argument.view()); !status.ok())
                return status;
        if (auto status = arguments.prepend_argv(script.interpreter.view()); !status.ok())
            return status;
        path = script.interpreter;
    }
}
} // namespace
i64 exec_image(Kernel &kernel, sched::ProcessId pid, sched::ProcessControlBlock *process,
               sched::ThreadId calling_tid, hw::AddressSpace &space, const std::array<uptr, 6> &a,
               bool &complete)
{
    FixedString<256> path;
    Arguments arguments;
    if (auto status = string_from_user(space, a[0], path); !status.ok())
        return error(status);
    if (auto status = arguments_from_user(space, a[1], arguments, false); !status.ok())
        return error(status);
    if (auto status = arguments_from_user(space, a[2], arguments, true); !status.ok())
        return error(status);
    if (auto status = resolve_scripts(kernel, path, arguments); !status.ok())
        return error(status);
    auto previous_image_retirement = ImageRetirementReservation::reserve();
    if (!previous_image_retirement) return error(previous_image_retirement.status());
    auto image = load(kernel, path.view(), arguments);
    if (!image)
        return error(image.status());
    if (auto status = process->resources().memory_limits.admit(image.value().pages.size(), hw::page_size); !status)
        return error(status); // The quota survives exec.
    auto context = kernel.arch().make_user_context({image.value().frame.pc, image.value().frame.sp});
#ifdef OK_PRIVATE_POLL_MASK
    const auto &mask_owner = *kernel.scheduler().find_thread(pid, calling_tid);
    const auto inherited_mask = signal_waits.find(mask_owner) ? signal_waits.exec_mask(mask_owner)
                                                            : poll_signal_masks.exec_mask(mask_owner);
#else
    const auto inherited_mask = kernel.scheduler().find_thread(pid, calling_tid)->native_signal_mask;
#endif
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__arm__) || defined(__riscv) || defined(__loongarch64) || defined(__mips__) || defined(__powerpc__) || defined(__PPC__))
    auto *pending_before_exec = private_signal_queue(*kernel.scheduler().find_thread(pid, calling_tid));
    const auto altstack_before_exec = *private_signal_altstack(*kernel.scheduler().find_thread(pid, calling_tid));
    std::array<RetainedEntryStack, sched::max_threads_per_process> old_signal_owners{};
    usize old_signal_owner_count{};
    for (const auto &old_thread : process->threads())
        if (old_thread.entry_stack.pool)
            old_signal_owners[old_signal_owner_count++] = {{pid, old_thread.tid}, old_thread.entry_stack, 0};
#endif
    ExecRetirement retirement{*process, calling_tid};
    VfsImageRetirement vfs_retirement;
    if (auto status = vfs_retirement.prepare(kernel.scheduler(), *process); !status)
    {
        return error(status);
    }
#if defined(OK_PRIVATE_MIPS64_N64_SIGNAL) || defined(OK_PRIVATE_PPC32_SIGNAL)
    std::array<u32, sched::max_threads_per_process> previous_signal_slots{};
    usize previous_signal_count{};
    for (const auto &old_thread : process->threads())
        if (old_thread.entry_stack.pool) previous_signal_slots[previous_signal_count++] = old_thread.entry_stack.slot;
#endif
    bool vfs_retired{};
    const auto replaced = replace_entry_stacks(entry_stacks, *process,
        [&] {
            return kernel.posix().exec(pid, context, image.value().memory);
        }, [&] {
            // Scheduler commit has escrowed the old entry stacks. A failed
            // futex discharge must retain them and both VM generations.
            if (auto status = cancel_futex(kernel, pid); !status) return status;
            const auto native_udp_retired = retire_native_udp_receives_for_exec(kernel, pid);
            const auto linux_udp_retired = retire_linux_udp_receives_for_exec(kernel, pid);
            if (!native_udp_retired) return native_udp_retired;
            if (!linux_udp_retired) return linux_udp_retired;
            const auto status = vfs_retirement.consume(kernel.scheduler(), *process);
            vfs_retired = status.ok();
            if (!status) return status;
#ifdef OK_PRIVATE_POLL_MASK
            {
                for (const auto &wait : signal_waits.records())
                    if (wait.phase != PrivateSignalWaitState::Phase::none && wait.pid == pid)
                        private_signal_wait_trace_retire(wait, "exec");
                poll_signal_masks.discard_process(pid); signal_waits.discard_process(pid);
            }
#endif
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
            for (usize i = 0; i < previous_signal_count; ++i) private_n64_revoke_slot(previous_signal_slots[i]);
#endif
#ifdef OK_PRIVATE_PPC32_SIGNAL
            for (usize i = 0; i < previous_signal_count; ++i) private_ppc_revoke_slot(previous_signal_slots[i]);
#endif
            return status;
        });
    if (replaced.committed && vfs_retired) retirement.consume(kernel, *process);
    if (!replaced)
    {
        if (!replaced.committed)
        {
            return error(replaced.status);
        }
        // Exec cannot return to the old image after scheduler commit. Keep
        // both VM generations and every unreleased lease for diagnosis.
        for (usize i = 0; i < replaced.retained_count; ++i)
        {
            const auto &saved = replaced.retained[i];
            stranded_entry_stacks[saved.handle.slot] = {saved.owner, saved.handle};
        }
        static_cast<void>(image.value().retain_for_diagnosis());
        runtime_failure = replaced.status;
        started = false;
        complete = false;
        return 0;
    }
    cancel_poll(kernel, pid);
    if (!runtime_failure)
    {
        static_cast<void>(image.value().retain_for_diagnosis());
        started = false;
        complete = false;
        return 0;
    }
    if (auto status = retire_then_install(process->resources(), image.value(), previous_image_retirement.value(),
            [&] { return install(*process, image.value(), inherited_mask); }); !status)
    {
        runtime_failure = status;
        started = false;
        complete = false;
        return 0;
    }
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__arm__) || defined(__riscv) || defined(__loongarch64) || defined(__mips__) || defined(__powerpc__) || defined(__PPC__))
    private_signal_exec(kernel, *process, pending_before_exec, altstack_before_exec);
    for (usize index = 0; index < old_signal_owner_count; ++index)
        if (old_signal_owners[index].handle != process->threads()[0].entry_stack)
            private_signal_thread_retire(pid, old_signal_owners[index].owner.tid, old_signal_owners[index].handle);
#endif
    complete = false; // The new image has its own entry register state.
    return 0;
}
Result<sched::ProcessId> install_init(Kernel &kernel, const fs::BootArchive &archive)
{
    Arguments arguments;
    if (auto status = arguments.push_argv(archive.initial_program); !status.ok())
        return status;
    if (auto status = arguments.push_envp("PATH=/bin"); !status.ok())
        return status;
    const auto initial_credentials = archive.initial_credentials;
    if (initial_credentials.kernel_space || initial_credentials.uid != initial_credentials.euid ||
        initial_credentials.gid != initial_credentials.egid ||
        initial_credentials.suid != initial_credentials.euid ||
        initial_credentials.sgid != initial_credentials.egid)
        return Status::invalid_argument("initial user identity requires equal unprivileged IDs");
    if (auto status = kernel.posix().set_credentials(initial_credentials); !status)
        return status;
    auto image = load(kernel, archive.initial_program, arguments);
    const auto restored = kernel.posix().set_credentials(user::root_credentials());
    if (!restored)
    {
        return restored;
    }
    if (!image)
        return image.status();
    auto context = kernel.arch().make_user_context({image.value().frame.pc, image.value().frame.sp});
    auto pid = kernel.scheduler().create_init_process(context);
    if (!pid)
    {
        return pid.status();
    }
    auto *process = kernel.scheduler().find(pid.value());
    process->set_credentials(initial_credentials);
    if (auto status = prepare_native_thread(process->threads()[0]); !status)
    {
        finish(kernel, pid.value(), 127);
        return status;
    }
    if (auto status = install(*process, image.value()); !status)
    {
        finish(kernel, pid.value(), 127);
        return status;
    }
#ifdef OK_PRIVATE_POLL_MASK
    if (!private_signal_process(kernel, pid.value()))
    {
        finish(kernel, pid.value(), 127);
        return Status::no_memory("private process signal owner capacity");
    }
#endif
    return pid.value();
}
} // namespace runtime_detail
} // namespace ok::user::native
