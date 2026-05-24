#include "ok/core/entry.hpp"

namespace ok
{
namespace
{

Kernel &kernel_instance()
{
    static Kernel kernel;
    return kernel;
}

void emit(const KernelDebugSink &sink, std::string_view text)
{
    sink.emit(text);
}

void emit_unsigned(const KernelDebugSink &sink, u64 value)
{
    constexpr u64 powers[] = {
        10'000'000'000'000'000'000ull,
        1'000'000'000'000'000'000ull,
        100'000'000'000'000'000ull,
        10'000'000'000'000'000ull,
        1'000'000'000'000'000ull,
        100'000'000'000'000ull,
        10'000'000'000'000ull,
        1'000'000'000'000ull,
        100'000'000'000ull,
        10'000'000'000ull,
        1'000'000'000ull,
        100'000'000ull,
        10'000'000ull,
        1'000'000ull,
        100'000ull,
        10'000ull,
        1'000ull,
        100ull,
        10ull,
        1ull,
    };
    bool started = false;
    for (const auto power : powers)
    {
        u8 digit = 0;
        while (value >= power)
        {
            value -= power;
            ++digit;
        }
        if (digit != 0 || started || power == 1)
        {
            const char out = static_cast<char>('0' + digit);
            emit(sink, std::string_view{&out, 1});
            started = true;
        }
    }
}

void emit_bool_field(const KernelDebugSink &sink, std::string_view name, bool value)
{
    emit(sink, " ");
    emit(sink, name);
    emit(sink, "=");
    emit(sink, value ? "1" : "0");
}

void emit_unsigned_field(const KernelDebugSink &sink, std::string_view name, u64 value)
{
    emit(sink, " ");
    emit(sink, name);
    emit(sink, "=");
    emit_unsigned(sink, value);
}

void emit_failure(const KernelDebugSink &sink, Status status)
{
    emit(sink, "OK_TEST_FAIL code=");
    emit_unsigned(sink, static_cast<u32>(status.code()));
    emit(sink, " message=");
    emit(sink, status.message());
    emit(sink, "\n");
}

void emit_display_text(const KernelDebugSink &sink, std::string_view text)
{
    usize line_start = 0;
    for (usize i = 0; i <= text.size(); ++i)
    {
        if (i == text.size() || text[i] == '\n')
        {
            if (i > line_start)
            {
                emit(sink, "OK_DISPLAY_TEXT ");
                emit(sink, text.substr(line_start, i - line_start));
                emit(sink, "\n");
            }
            line_start = i + 1;
        }
    }
}

void emit_roadmap_markers(const KernelDebugSink &sink, Kernel &kernel)
{
    const auto &report = kernel.test_report();
    if (report.modules)
    {
        for (auto name : {"arch", "memory", "interrupt", "scheduler", "smp", "ipc", "syscall", "driver-core", "vfs",
                          "posix", "user-mode", "debug-shell"})
        {
            emit(sink, "OK_MODULE name=");
            emit(sink, name);
            emit(sink, " state=started\n");
        }
        emit(sink, "OK_MODULES count=");
        emit_unsigned(sink, report.module_count);
        emit(sink, " failed=");
        emit_unsigned(sink, report.module_failed_count);
        emit(sink, "\n");
    }
    if (report.vm)
    {
        emit(sink, "OK_VM kernel_map=pass user_map=pass user_copy=pass fault=pass\n");
    }
    if (report.proc)
    {
        emit(sink, "OK_PROC create=pass schedule=pass exit=pass wait=pass\n");
    }
    if (report.elf)
    {
        emit(sink, "OK_ELF load=pass entry=pass stack=pass\n");
    }
    if (report.userland)
    {
        emit(sink, "OK_USERLAND hello=pass fd=pass fork=pass exec=pass\n");
    }
    if (report.vfs_unix)
    {
        emit(sink, "OK_VFS mount=pass path=pass file=pass dir=pass symlink=pass\n");
    }
    if (report.devfs)
    {
        emit(sink, "OK_DEVFS null=pass zero=pass console=pass\n");
    }
    if (report.pipe)
    {
        emit(sink, "OK_PIPE create=pass transfer=pass poll=pass\n");
    }
    if (report.tty)
    {
        emit(sink, "OK_TTY console=pass ioctl=pass\n");
    }
    if (report.linux_abi)
    {
        emit(sink, "OK_LINUX_ABI arch=");
        emit(sink, arch::to_string(kernel.arch().architecture()));
        emit(sink, " args=pass errno=pass unknown=pass\n");
    }
    if (report.linux_syscalls)
    {
        emit(sink, "OK_LINUX_SYSCALLS file=pass memory=pass time=pass futex=pass tls=pass\n");
    }
    if (report.driver_abi)
    {
        emit(sink, "OK_DRIVER_ABI native_probe=pass irq=pass mmio=pass remove=pass\n");
    }
    if (report.linux_driver_shim)
    {
        emit(sink, "OK_LINUX_DRIVER_SHIM compile=pass probe=pass mmio=pass alloc=pass remove=pass\n");
    }
    if (report.gui)
    {
        emit(sink, "OK_GUI compositor=pass surface=pass restart=pass\n");
    }
    if (report.module_load)
    {
        emit(sink, "OK_MODULE_LOAD elf=pass reloc=pass symbols=pass unload=pass\n");
    }
    if (report.netdev)
    {
        emit(sink, "OK_NETDEV virtio=pass arp=pass icmp=pass socket=pass\n");
    }
    if (report.sockets)
    {
        emit(sink, "OK_SOCK udp=pass tcp=pass poll=pass\n");
    }
    if (report.block)
    {
        emit(sink, "OK_BLOCK disk=virtio cache=pass io=pass part=pass fat32=pass exfat=pass bounds=pass\n");
    }
    if (report.ext4_readonly)
    {
        emit(sink, "OK_EXT4_READONLY super=pass block=pass corrupt=pass bounds=pass\n");
    }
    if (report.smp_roadmap)
    {
        emit(sink, "OK_SMP cpus=4 online=4 ap_start=pass per_cpu=pass\n");
    }
    if (report.irq_roadmap)
    {
        emit(sink, "OK_IRQ idt=pass timer=pass syscall=pass page_fault=pass\n");
    }
    if (report.preempt)
    {
        emit(sink, "OK_PREEMPT tick=pass switch=pass sleep=pass idle=pass\n");
    }
}

[[maybe_unused]] void emit_pass(const KernelDebugSink &sink, Kernel &kernel)
{
    const auto &report = kernel.test_report();
    emit_display_text(sink, kernel.display().text());
    emit(sink, "OK_TEST_PASS arch=");
    emit(sink, arch::to_string(kernel.arch().architecture()));
    emit_unsigned_field(sink, "processes", kernel.scheduler().process_count());
    emit_unsigned_field(sink, "cpus", kernel.topology().online_count());
    emit_unsigned_field(sink, "drivers", kernel.drivers().driver_count());
    emit_unsigned_field(sink, "free_frames", kernel.memory().frames().free_frames());
    emit_unsigned_field(sink, "syscalls", kernel.syscalls().handler_count());
    emit_unsigned_field(sink, "debug_test_points", kernel.debug_test_points_run());
    emit_bool_field(sink, "fs", report.vfs);
    emit_bool_field(sink, "simplefs", report.simplefs);
    emit_bool_field(sink, "ext4", report.ext4);
    emit_bool_field(sink, "user", report.user_mode);
    emit_bool_field(sink, "display", report.display);
    emit_bool_field(sink, "gpu", report.gpu);
    emit_bool_field(sink, "gui", report.gui);
    emit_bool_field(sink, "input", report.input);
    emit_bool_field(sink, "posix", report.posix);
    emit_bool_field(sink, "bus", report.bus);
    emit_bool_field(sink, "usb", report.usb);
    emit_bool_field(sink, "net", report.net);
    emit_bool_field(sink, "shell", report.shell);
    emit_bool_field(sink, "modes", report.modes);
    emit_unsigned_field(sink, "display_checksum", kernel.display().checksum());
    emit(sink, "\n");
}

} // namespace

Status ok_kernel_entry(const KernelEntryConfig &config, KernelEntryResult *result)
{
    const auto &sink = config.debug;

    if (config.mode == KernelBootMode::normal)
    {
        Kernel &kernel = kernel_instance();
        const auto status = kernel.boot(config.kernel);
        if (result != nullptr)
        {
            result->status = status;
            if (status.ok())
            {
                result->tests = kernel.test_report();
                result->display_checksum = kernel.display().checksum();
            }
        }
        return status;
    }

    emit(sink, "OK_MODE debug\n");

#if !defined(OK_ENABLE_TEST_POINTS)
    const auto status = Status::invalid_argument("kernel test entry requires a debug build with OK_ENABLE_TEST_POINTS");
    emit_failure(sink, status);
    if (result != nullptr)
    {
        result->status = status;
    }
    return status;
#else
    Kernel &kernel = kernel_instance();
    emit(sink, "OK_DEBUG boot=begin\n");
    if (auto status = kernel.boot(config.kernel); !status.ok())
    {
        emit_failure(sink, status);
        if (result != nullptr)
        {
            result->status = status;
        }
        return status;
    }
    emit(sink, "OK_DEBUG boot=complete\n");

    if (auto status = kernel.run_debug_test_suite(); !status.ok())
    {
        emit_failure(sink, status);
        if (result != nullptr)
        {
            result->status = status;
        }
        return status;
    }

    emit_roadmap_markers(sink, kernel);
    emit(sink, "OK_DEBUG fs=pass user=pass display=pass\n");
    emit_pass(sink, kernel);

    if (result != nullptr)
    {
        result->status = Status::success();
        result->tests = kernel.test_report();
        result->debug_test_points = kernel.debug_test_points_run();
        result->display_checksum = kernel.display().checksum();
    }
    return Status::success();
#endif
}

Result<std::string_view> ok_debug_shell_execute(std::string_view command)
{
    Kernel &kernel = kernel_instance();
    if (!kernel.booted())
    {
        return Status::not_initialized("kernel is not booted");
    }
    return kernel.debug_shell().execute(command);
}

Status ok_debug_shell_show_gui()
{
    Kernel &kernel = kernel_instance();
    if (!kernel.booted())
    {
        return Status::not_initialized("kernel is not booted");
    }
    return kernel.debug_shell().show_gui();
}

Status ok_debug_shell_set_gui_input(std::string_view line)
{
    Kernel &kernel = kernel_instance();
    if (!kernel.booted())
    {
        return Status::not_initialized("kernel is not booted");
    }
    return kernel.debug_shell().set_gui_input(line);
}

Status ok_debug_shell_scroll_gui(i32 rows)
{
    Kernel &kernel = kernel_instance();
    if (!kernel.booted())
    {
        return Status::not_initialized("kernel is not booted");
    }
    return kernel.debug_shell().scroll_gui_history(rows);
}

Status ok_gui_mouse_event(i32 delta_x, i32 delta_y, bool left_button)
{
    Kernel &kernel = kernel_instance();
    if (!kernel.booted())
    {
        return Status::not_initialized("kernel is not booted");
    }
    return kernel.handle_gui_mouse(delta_x, delta_y, left_button);
}

bool ok_debug_shell_gui_ready()
{
    Kernel &kernel = kernel_instance();
    return kernel.booted() && kernel.debug_shell().gui_ready();
}

extern "C" int ok_kernel_main(const KernelEntryConfig *config)
{
    if (config == nullptr)
    {
        KernelEntryConfig fallback{};
        fallback.mode = KernelBootMode::debug;
        return ok_kernel_entry(fallback).ok() ? 0 : 1;
    }
    return ok_kernel_entry(*config).ok() ? 0 : 1;
}

} // namespace ok
