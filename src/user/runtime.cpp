#include "ok/user/runtime.hpp"
#include "ok/arch/execution.hpp"
#include "ok/core/kernel.hpp"
#include "ok/sched/process.hpp"
#include "ok/syscall/linux.hpp"

extern "C" ok::Status ok_boot_filesystem(ok::fs::VirtualFileSystem &) __attribute__((weak));

namespace ok::user::native
{
namespace
{
namespace hw = arch::execution;
void copy_bytes(void *dst, const void *src, usize size)
{
    auto *out = static_cast<std::byte *>(dst);
    auto *in = static_cast<const std::byte *>(src);
    for (usize i = 0; i < size; ++i)
        out[i] = in[i];
}
using Pages = StaticVector<hw::PageMapping, 128>;
constexpr usize page_count = 1024;
alignas(4096) std::array<std::array<std::byte, hw::page_size>, page_count> physical_pages;
std::array<bool, page_count> used_pages{};
std::array<std::byte, 128 * 1024> elf_buffer{};
sched::ProcessId last_pid{};
bool started{};
std::array<char, 256> input_buffer{};
usize input_read{}, input_write{};
extern "C" void ok_platform_console_write_char(char) __attribute__((weak));
void emit(std::string_view text)
{
    if (ok_platform_console_write_char != nullptr)
        for (char c : text)
            ok_platform_console_write_char(c);
}
void release(hw::AddressSpace &space, Pages &pages)
{
    if (space.root != 0)
        static_cast<void>(hw::destroy_space(space));
    for (const auto &page : pages)
    {
        auto base = reinterpret_cast<uptr>(physical_pages.data());
        if (page.physical_address >= base && page.physical_address - base < sizeof(physical_pages))
            used_pages[(page.physical_address - base) / hw::page_size] = false;
    }
    pages.clear();
}
Status allocate(Pages &pages, uptr address, u8 flags)
{
    for (const auto &page : pages)
        if (page.virtual_address == address)
            return Status::invalid_argument("ELF pages overlap");
    if (pages.full())
        return Status::no_memory("process page limit exceeded");
    for (usize i = 0; i < page_count; ++i)
        if (!used_pages[i])
        {
            used_pages[i] = true;
            physical_pages[i].fill(std::byte{});
            return pages.push_back({address, reinterpret_cast<uptr>(physical_pages[i].data()), flags});
        }
    return Status::no_memory("native physical page pool exhausted");
}
Status store(Pages &pages, uptr address, const void *data, usize count)
{
    const auto *bytes = static_cast<const std::byte *>(data);
    while (count)
    {
        hw::PageMapping *mapping = nullptr;
        for (auto &page : pages)
            if (page.virtual_address == (address & ~(hw::page_size - 1)))
            {
                mapping = &page;
                break;
            }
        if (!mapping)
            return Status::fault("image destination is unmapped");
        usize offset = address & (hw::page_size - 1);
        usize chunk = hw::page_size - offset;
        if (chunk > count)
            chunk = count;
        copy_bytes(reinterpret_cast<void *>(mapping->physical_address + offset), bytes, chunk);
        address += chunk;
        bytes += chunk;
        count -= chunk;
    }
    return Status::success();
}
struct Arguments
{
    StaticVector<FixedString<256>, 16> argv;
    StaticVector<FixedString<256>, 16> envp;
};
struct Image
{
    Pages pages;
    hw::AddressSpace space;
    hw::UserFrame frame;
    sched::MemoryMap memory;
};
Result<Image> load(Kernel &kernel, std::string_view path, const Arguments &args)
{
    auto *node = kernel.vfs().lookup(path);
    if (!node)
        return Status::not_found("executable not found");
    const auto metadata = node->metadata();
    if (metadata.type != fs::NodeType::regular || (metadata.mode & 0111u) == 0)
        return Status::denied("file is not executable");
    if (metadata.size > elf_buffer.size())
        return Status::overflow("executable exceeds loader capacity");
    usize offset = 0;
    while (offset < metadata.size)
    {
        auto bytes = node->read(offset, metadata.size - offset);
        if (!bytes)
            return bytes.status();
        if (bytes.value().size == 0)
            return Status::invalid_argument("truncated executable");
        copy_bytes(elf_buffer.data() + offset, bytes.value().data.data(), bytes.value().size);
        offset += bytes.value().size;
    }
    constexpr u8 native_encoding = __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__ ? 2 : 1;
    if (metadata.size < 16 || std::to_integer<u8>(elf_buffer[4]) != (sizeof(uptr) == 8 ? 2 : 1) ||
        std::to_integer<u8>(elf_buffer[5]) != native_encoding)
        return Status::invalid_argument("ELF class or byte order does not match the native ABI");
    auto elf = sched::ElfLoader{}.load({elf_buffer.data(), metadata.size}, kernel.arch().architecture());
    if (!elf)
        return elf.status();
    if (!elf.value().static_executable)
        return Status::unsupported("only static ET_EXEC images can run");
    Image image;
    auto fail = [&](Status status) -> Result<Image> {
        release(image.space, image.pages);
        return status;
    };
    bool entry_executable = false;
    constexpr uptr stack_top = hw::user_end - 0x100000;
    constexpr usize stack_size = 16 * hw::page_size;
    for (const auto &segment : elf.value().segments)
    {
        if (segment.memory_size == 0)
            continue;
        if (segment.virtual_address < hw::user_begin ||
            segment.virtual_address >= stack_top - stack_size - hw::page_size ||
            segment.memory_size > stack_top - stack_size - hw::page_size - segment.virtual_address ||
            segment.file_offset > metadata.size || segment.file_size > metadata.size - segment.file_offset)
            return fail(Status::invalid_argument("ELF segment outside user window"));
        u8 flags = hw::page_read;
        if (segment.permissions & memory::page_write)
            flags |= hw::page_write;
        if (segment.permissions & memory::page_execute)
            flags |= hw::page_execute;
        if ((flags & (hw::page_write | hw::page_execute)) == (hw::page_write | hw::page_execute))
            return fail(Status::denied("writable executable pages are forbidden"));
        if ((flags & hw::page_execute) && elf.value().entry >= segment.virtual_address &&
            elf.value().entry - segment.virtual_address < segment.memory_size)
            entry_executable = true;
        const uptr begin = segment.virtual_address & ~(hw::page_size - 1);
        const uptr end = (segment.virtual_address + segment.memory_size + hw::page_size - 1) & ~(hw::page_size - 1);
        if (auto status = image.memory.add_area({begin, end - begin, segment.permissions}); !status.ok())
            return fail(status);
        for (uptr address = begin; address < end; address += hw::page_size)
            if (auto status = allocate(image.pages, address, flags); !status.ok())
                return fail(status);
        if (auto status =
                store(image.pages, segment.virtual_address, elf_buffer.data() + segment.file_offset, segment.file_size);
            !status.ok())
            return fail(status);
    }
    if (!entry_executable)
        return fail(Status::invalid_argument("ELF entry is not executable"));
    for (uptr address = stack_top - stack_size; address < stack_top; address += hw::page_size)
        if (auto status = allocate(image.pages, address, hw::page_read | hw::page_write); !status.ok())
            return fail(status);
    static_cast<void>(image.memory.add_area(
        {stack_top - stack_size, stack_size, memory::page_user | memory::page_read | memory::page_write}));
    uptr sp = stack_top;
    std::array<uptr, 16> argv{}, envp{};
    auto strings = [&](const auto &values, auto &pointers) -> Status {
        for (usize i = 0; i < values.size(); ++i)
        {
            sp -= values[i].size() + 1;
            pointers[i] = sp;
            if (auto status = store(image.pages, sp, values[i].c_str(), values[i].size() + 1); !status.ok())
                return status;
        }
        return Status::success();
    };
    if (auto status = strings(args.argv, argv); !status.ok())
        return fail(status);
    if (auto status = strings(args.envp, envp); !status.ok())
        return fail(status);
    std::array<uptr, 48> words{};
    usize n = 0;
    words[n++] = args.argv.size();
    for (usize i = 0; i < args.argv.size(); ++i)
        words[n++] = argv[i];
    words[n++] = 0;
    for (usize i = 0; i < args.envp.size(); ++i)
        words[n++] = envp[i];
    words[n++] = 0;
    words[n++] = 6;
    words[n++] = hw::page_size; // AT_PAGESZ
    words[n++] = 9;
    words[n++] = elf.value().entry; // AT_ENTRY
    words[n++] = 0;
    words[n++] = 0;
    sp = (sp - n * sizeof(uptr)) & ~uptr(15);
    if (auto status = store(image.pages, sp, words.data(), n * sizeof(uptr)); !status.ok())
        return fail(status);
    auto space = hw::create_space({image.pages.begin(), image.pages.size()});
    if (!space)
        return fail(space.status());
    image.space = space.value();
    image.frame = hw::initial_frame(elf.value().entry, sp);
    return image;
}
Status string_from_user(const hw::AddressSpace &space, uptr address, FixedString<256> &result)
{
    result.clear();
    for (usize i = 0; i < 256; ++i)
    {
        char c;
        if (address + i < address || !hw::copy_from_user(space, &c, address + i, 1))
            return Status::fault("invalid user string");
        if (c == 0)
            return Status::success();
        if (auto status = result.append(c); !status.ok())
            return status;
    }
    return Status::overflow("user string exceeds limit");
}
Status arguments_from_user(const hw::AddressSpace &space, uptr pointer, auto &out)
{
    if (pointer == 0)
        return Status::success();
    for (usize i = 0; i <= 16; ++i)
    {
        uptr value{};
        const uptr address = pointer + i * sizeof(uptr);
        if (address < pointer || !hw::copy_from_user(space, &value, address, sizeof(value)))
            return Status::fault("invalid argument vector");
        if (value == 0)
            return Status::success();
        if (i == 16)
            return Status::overflow("too many arguments");
        FixedString<256> text;
        if (auto status = string_from_user(space, value, text); !status.ok())
            return status;
        static_cast<void>(out.push_back(text));
    }
    return Status::overflow("argument vector exceeds limit");
}
void install(sched::ProcessControlBlock &process, Image &image)
{
    // Native bridges currently execute only on the boot CPU.
    process.set_cpu_affinity_mask(1);
    auto &resources = process.resources();
    resources.native_space = image.space;
    resources.native_pages = image.pages;
    auto &thread = process.threads()[0];
    thread.native_frame = image.frame;
    thread.native_active = true;
    thread.syscall_pending = false;
    process.memory_map() = image.memory;
}
i64 error(Status status)
{
    return -syscall::ErrnoMapper::errno_for(status);
}
void finish(Kernel &kernel, sched::ProcessId pid, i32 code, u8 signal = 0)
{
    auto *process = kernel.scheduler().find(pid);
    if (!process)
        return;
    release(process->resources().native_space, process->resources().native_pages);
    for (auto &thread : process->threads())
        thread.native_active = false;
    if (signal != 0)
        static_cast<void>(kernel.scheduler().terminate_process(pid, signal));
    else
        static_cast<void>(kernel.posix().exit(pid, code));
}
// Only this boundary accepts untrusted user pointers. The existing syscall
// table receives kernel-owned buffers; calls without a complete marshaller are
// deliberately unavailable to native programs.
i64 dispatch(Kernel &kernel, sched::ProcessId pid, hw::Syscall call, bool &complete)
{
    auto *process = kernel.scheduler().find(pid);
    auto &space = process->resources().native_space;
    using N = syscall::Number;
    const auto number = static_cast<N>(call.number);
    const auto &a = call.arguments;
    if (number == N::exit || number == N::exit_group)
    {
        finish(kernel, pid, static_cast<i32>(a[0]));
        complete = false;
        return 0;
    }
    if (number == N::fork)
    {
        auto result = kernel.posix().fork(pid);
        if (!result)
            return error(result.status());
        const auto child_pid = result.value();
        auto *child = kernel.scheduler().find(child_pid);
        auto &pages = child->resources().native_pages;
        for (const auto &page : process->resources().native_pages)
        {
            if (auto status = allocate(pages, page.virtual_address, page.flags); !status.ok())
            {
                finish(kernel, child_pid, 127);
                static_cast<void>(kernel.posix().wait(pid, child_pid));
                return error(status);
            }
            copy_bytes(reinterpret_cast<void *>(pages[pages.size() - 1].physical_address),
                       reinterpret_cast<const void *>(page.physical_address), hw::page_size);
        }
        auto child_space = hw::create_space({pages.begin(), pages.size()});
        if (!child_space)
        {
            finish(kernel, child_pid, 127);
            static_cast<void>(kernel.posix().wait(pid, child_pid));
            return error(child_space.status());
        }
        child->resources().native_space = child_space.value();
        child->threads()[0].native_active = true;
        child->threads()[0].syscall_pending = false;
        hw::set_return_value(child->threads()[0].native_frame, 0);
        return static_cast<i64>(child_pid);
    }
    if (number == N::execve)
    {
        FixedString<256> path;
        Arguments arguments;
        if (auto status = string_from_user(space, a[0], path); !status.ok())
            return error(status);
        if (auto status = arguments_from_user(space, a[1], arguments.argv); !status.ok())
            return error(status);
        if (auto status = arguments_from_user(space, a[2], arguments.envp); !status.ok())
            return error(status);
        auto image = load(kernel, path.view(), arguments);
        if (!image)
            return error(image.status());
        auto context = kernel.arch().make_user_context({image.value().frame.pc, image.value().frame.sp});
        if (auto status = kernel.posix().exec(pid, context, image.value().memory); !status.ok())
        {
            release(image.value().space, image.value().pages);
            return error(status);
        }
        release(space, process->resources().native_pages);
        install(*process, image.value());
        complete = false; // The new image has its own entry register state.
        return 0;
    }
    if (number == N::wait4)
    {
        if (a[2] & ~uptr(1) || a[3] != 0)
            return -22;
        if (a[1] != 0 && !hw::user_range(space, a[1], sizeof(i32), true))
            return -14;
        sched::ProcessId child_pid = static_cast<sched::ProcessId>(a[0]);
        if (static_cast<i32>(a[0]) == -1)
        {
            child_pid = 0;
            for (auto child : process->children())
            {
                const auto *candidate = kernel.scheduler().find(child);
                if (candidate && candidate->state() == sched::ProcessState::zombie)
                {
                    child_pid = child;
                    break;
                }
                if (!child_pid)
                    child_pid = child;
            }
        }
        const auto *child = kernel.scheduler().find(child_pid);
        if (!child || child->parent_pid() != pid)
            return -10; // ECHILD
        if ((a[2] & 1) && child->state() != sched::ProcessState::zombie)
            return 0;
        const i32 status = child->wait_status();
        auto result = kernel.posix().wait(pid, child_pid);
        if (!result)
        {
            if (result.status().code() == StatusCode::would_block)
            {
                complete = false;
                if (static_cast<i32>(a[0]) == -1)
                    process->threads()[0].wait_key = 0;
            }
            return error(result.status());
        }
        // Reaping may compact the canonical process array.
        process = kernel.scheduler().find(pid);
        if (a[1])
            static_cast<void>(hw::copy_to_user(process->resources().native_space, a[1], &status, sizeof(status)));
        return static_cast<i64>(child_pid);
    }
    if (number == N::pipe)
    {
        if (!hw::user_range(space, a[0], sizeof(i32) * 2, true))
            return -14;
        auto fds = kernel.posix().pipe();
        if (!fds)
            return error(fds.status());
        static_cast<void>(hw::copy_to_user(space, a[0], fds.value().data(), sizeof(i32) * 2));
        return 0;
    }
    if (number == N::readv || number == N::writev)
    {
        struct Vector { uptr base; usize length; };
        std::array<Vector, 16> vectors{};
        if (a[2] > vectors.size()) return -22;
        if (!hw::copy_from_user(space, vectors.data(), a[1], a[2] * sizeof(Vector))) return -14;
        const auto *slot = process->fd_table().entry(static_cast<i32>(a[0]));
        auto *description = slot && slot->used ? kernel.scheduler().open_description(slot->description) : nullptr;
        const bool writing = number == N::writev;
        if (!description || !(writing ? description->writable : description->readable)) return -9;
        usize total = 0;
        for (usize i = 0; i < a[2]; ++i)
        {
            if (vectors[i].length > static_cast<usize>(__INTPTR_MAX__) - total) return -22;
            total += vectors[i].length;
            if (!hw::user_range(space, vectors[i].base, vectors[i].length, !writing)) return -14;
        }
        if (!total) return 0;
        std::array<std::byte, 4096> data{};
        if (total > data.size()) total = data.size();
        usize position = 0;
        if (writing)
            for (usize i = 0; i < a[2] && position < total; ++i)
            {
                auto length = vectors[i].length < total - position ? vectors[i].length : total - position;
                if (!hw::copy_from_user(space, data.data() + position, vectors[i].base, length)) return -14;
                position += length;
            }
        Result<usize> result = usize{0};
        if (writing && description->console)
        {
            emit({reinterpret_cast<const char *>(data.data()), total});
            result = kernel.posix().write(static_cast<i32>(a[0]), {data.data(), total});
        }
        else if (!writing && description->input_console)
        {
            usize count = 0;
            while (count < total && input_read != input_write)
                data[count++] = static_cast<std::byte>(input_buffer[input_read++ % input_buffer.size()]);
            result = count ? Result<usize>{count} : Result<usize>{Status::would_block()};
        }
        else if (writing)
            result = kernel.posix().write(static_cast<i32>(a[0]), {data.data(), total});
        else
            result = kernel.posix().read(static_cast<i32>(a[0]), {data.data(), total});
        if (!result)
        {
            if (result.status().code() == StatusCode::would_block) complete = false;
            return error(result.status());
        }
        if (!writing)
        {
            position = 0;
            for (usize i = 0; i < a[2] && position < result.value(); ++i)
            {
                auto length = vectors[i].length < result.value() - position ? vectors[i].length : result.value() - position;
                if (!hw::copy_to_user(space, vectors[i].base, data.data() + position, length)) return -14;
                position += length;
            }
        }
        return static_cast<i64>(result.value());
    }
    if ((number == N::pread64 || number == N::pwrite64) && static_cast<__INTPTR_TYPE__>(a[3]) < 0)
        return -22;
    if ((number == N::truncate || number == N::ftruncate) && static_cast<__INTPTR_TYPE__>(a[1]) < 0)
        return -22;
    syscall::Request request{.number = number, .caller = pid};
    for (usize i = 0; i < 6; ++i)
        request.args[i] = a[i];
    if (number == N::lseek)
        request.args[1] = static_cast<u64>(static_cast<__INTPTR_TYPE__>(a[1]));
    alignas(16) std::array<std::byte, 4096> buffer{};
    FixedString<256> path;
    int path_arg = -1, output_arg = -1, input_arg = -1;
    usize output_size = 0, input_size = 0;
    bool variable_output = false;
    switch (number)
    {
    case N::read:
    case N::pread64:
    case N::getdents:
    case N::getdents64:
        output_arg = 1;
        output_size = a[2] < buffer.size() ? a[2] : buffer.size();
        request.args[2] = output_size;
        variable_output = true;
        break;
    case N::write:
    case N::pwrite64:
        input_arg = 1;
        input_size = a[2] < buffer.size() ? a[2] : buffer.size();
        request.args[2] = input_size;
        break;
    case N::truncate:
    case N::open:
    case N::creat:
    case N::mkdir:
    case N::unlink:
    case N::rmdir:
    case N::chdir:
    case N::access:
        path_arg = 0;
        break;
    case N::openat:
    case N::mkdirat:
    case N::unlinkat:
    case N::faccessat:
    case N::faccessat2:
        path_arg = 1;
        break;
    case N::stat:
    case N::lstat:
        path_arg = 0;
        output_arg = 1;
        output_size = sizeof(posix::FileStatus);
        break;
    case N::fstat:
        output_arg = 1;
        output_size = sizeof(posix::FileStatus);
        break;
    case N::newfstatat:
        path_arg = 1;
        output_arg = 2;
        output_size = sizeof(posix::FileStatus);
        break;
    case N::getcwd:
        output_arg = 0;
        output_size = a[1] < buffer.size() ? a[1] : buffer.size();
        request.args[1] = output_size;
        break;
    case N::clock_gettime:
        output_arg = 1;
        output_size = sizeof(posix::ClockTime);
        break;
    case N::clock_getres:
        if (a[1])
        {
            output_arg = 1;
            output_size = sizeof(posix::ClockTime);
        }
        break;
    case N::time:
        if (a[0])
        {
            output_arg = 0;
            output_size = sizeof(i64);
        }
        break;
    case N::gettimeofday:
        if (a[0])
        {
            output_arg = 0;
            output_size = sizeof(posix::ClockTime);
        }
        // The legacy timezone argument is ignored by the kernel service.
        request.args[1] = 0;
        break;
    case N::uname:
        output_arg = 0;
        output_size = sizeof(posix::UnameInfo);
        break;
    case N::getrlimit:
        output_arg = 1;
        output_size = sizeof(posix::ResourceLimit);
        break;
    case N::sysinfo:
        output_arg = 0;
        output_size = sizeof(posix::SystemInfo);
        break;
    case N::getrandom:
        output_arg = 0;
        output_size = a[1] < buffer.size() ? a[1] : buffer.size();
        request.args[1] = output_size;
        variable_output = true;
        break;
    case N::fsync:
    case N::fdatasync:
    case N::ftruncate:
    case N::close:
    case N::close_range:
    case N::dup:
    case N::dup2:
    case N::dup3:
    case N::lseek:
    case N::getpid:
    case N::getppid:
    case N::gettid:
    case N::getuid:
    case N::getgid:
    case N::geteuid:
    case N::getegid:
    case N::umask:
    case N::fcntl:
    case N::fchdir:
    case N::sched_yield:
        break;
    default:
        return -38;
    }
    if (path_arg >= 0)
    {
        if (auto status = string_from_user(space, a[path_arg], path); !status.ok())
            return error(status);
        request.args[path_arg] = reinterpret_cast<uptr>(path.c_str());
    }
    if (input_arg >= 0)
    {
        if (!hw::copy_from_user(space, buffer.data(), a[input_arg], input_size))
            return -14;
        request.args[input_arg] = reinterpret_cast<uptr>(buffer.data());
    }
    if (output_arg >= 0)
    {
        if (!hw::user_range(space, a[output_arg], output_size, true))
            return -14;
        request.args[output_arg] = reinterpret_cast<uptr>(buffer.data());
    }
    const auto *fd = process->fd_table().entry(static_cast<i32>(a[0]));
    const auto *description = fd && fd->used ? kernel.scheduler().open_description(fd->description) : nullptr;
    if (number == N::write && description && description->console && description->writable)
    {
        emit({reinterpret_cast<const char *>(buffer.data()), input_size});
        static_cast<void>(kernel.console().write({reinterpret_cast<const char *>(buffer.data()), input_size}));
        return static_cast<i64>(input_size);
    }
    if (number == N::read && description && description->input_console)
    {
        usize count = 0;
        while (count < output_size && input_read != input_write)
        {
            buffer[count++] = static_cast<std::byte>(input_buffer[input_read++ % input_buffer.size()]);
        }
        if (count == 0 && output_size != 0)
        {
            complete = false;
            return -11;
        }
        if (!hw::copy_to_user(space, a[1], buffer.data(), count))
            return -14;
        return static_cast<i64>(count);
    }
    auto result = kernel.syscalls().dispatch(request);
    if (!result.status.ok())
    {
        if (result.status.code() == StatusCode::would_block)
            complete = false;
        return error(result.status);
    }
    if (output_arg >= 0)
    {
        if (variable_output)
            output_size = static_cast<usize>(result.value);
        if (output_size > buffer.size() || !hw::copy_to_user(space, a[output_arg], buffer.data(), output_size))
            return -14;
    }
    return result.value;
}
} // namespace
Status start(Kernel &kernel, fs::BootArchive archive)
{
    if (started)
        return Status::already_exists("native runtime already started");
    if (!hw::supported())
        return Status::unsupported("native execution backend is unavailable");
    if (!archive.data || !archive.initial_program)
        return Status::invalid_argument("missing boot archive");
    if (auto status = fs::mount_archive(kernel.vfs(), {archive.data, archive.size}); !status.ok())
        return status;
    if (ok_boot_filesystem != nullptr)
        if (auto status = ok_boot_filesystem(kernel.vfs()); !status.ok())
            return status;
    if (auto status = hw::initialize(); !status.ok())
        return status;
    Arguments arguments;
    static_cast<void>(arguments.argv.push_back(FixedString<256>(archive.initial_program)));
    static_cast<void>(arguments.envp.push_back(FixedString<256>("PATH=/bin")));
    auto image = load(kernel, archive.initial_program, arguments);
    if (!image)
        return image.status();
    auto context = kernel.arch().make_user_context({image.value().frame.pc, image.value().frame.sp});
    auto pid = kernel.scheduler().create_user_process("init", context);
    if (!pid)
    {
        release(image.value().space, image.value().pages);
        return pid.status();
    }
    auto *process = kernel.scheduler().find(pid.value());
    process->set_credentials(user::root_credentials());
    install(*process, image.value());
    static_cast<void>(kernel.scheduler().set_runnable(pid.value()));
    started = true;
    emit("OK_NATIVE scope=hardware init=loaded\n");
    return Status::success();
}
bool step(Kernel &kernel)
{
    // The outer loop also handles GUI/services when no native task is ready.
    if (!kernel.posix().activate(0))
        return false;
    if (!started)
        return false;
    static_cast<void>(kernel.scheduler().reap_orphans());
    sched::ProcessId chosen = 0, first = 0;
    for (const auto &candidate : kernel.scheduler().processes())
    {
        if (candidate.threads().empty() || !candidate.threads()[0].native_active ||
            (candidate.state() != sched::ProcessState::runnable && candidate.state() != sched::ProcessState::running))
            continue;
        if (!first || candidate.pid() < first)
            first = candidate.pid();
        if (candidate.pid() > last_pid && (!chosen || candidate.pid() < chosen))
            chosen = candidate.pid();
    }
    if (!chosen)
        chosen = first;
    if (!chosen)
        return false;
    last_pid = chosen;
    if (!kernel.posix().activate(chosen))
        return false;
    auto *process = kernel.scheduler().find(chosen);
    auto &thread = process->threads()[0];
    if (!thread.syscall_pending)
    {
        auto trap = hw::enter(process->resources().native_space, thread.native_frame);
        if (!trap)
        {
            finish(kernel, chosen, 127);
            emit("OK_NATIVE scope=hardware enter=failed\n");
        }
        else if (trap.value().kind == hw::TrapKind::syscall)
            thread.syscall_pending = true;
        else if (trap.value().kind == hw::TrapKind::timer)
        {
            ++thread.cpu_time_ticks;
            kernel.posix().advance_clock(10);
            static bool reported = false;
            if (!reported)
            {
                emit("OK_NATIVE scope=hardware timer=preempted\n");
                reported = true;
            }
        }
        else
        {
            emit("OK_NATIVE scope=hardware fault=contained\n");
            finish(kernel, chosen, 0, 11);
        }
    }
    if (thread.native_active && thread.syscall_pending)
    {
        const auto call = hw::syscall(thread.native_frame);
        bool complete = true;
        const auto result = dispatch(kernel, chosen, call, complete);
        process = kernel.scheduler().find(chosen);
        if (process && process->threads()[0].native_active && complete)
        {
            auto &current = process->threads()[0];
            current.syscall_pending = false;
            hw::set_return_value(current.native_frame, result);
        }
    }
    static_cast<void>(kernel.posix().activate(0));
    static_cast<void>(kernel.scheduler().reap_orphans());
    return true;
}
void release_process(Kernel &kernel, u64 pid)
{
    if (auto *process = kernel.scheduler().find(pid))
    {
        release(process->resources().native_space, process->resources().native_pages);
        for (auto &thread : process->threads())
            thread.native_active = false;
    }
}
void input(char value)
{
    if (input_write - input_read < input_buffer.size())
        input_buffer[input_write++ % input_buffer.size()] = value;
}
} // namespace ok::user::native
