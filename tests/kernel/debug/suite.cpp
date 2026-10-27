#include "ok/core/entry.hpp"
#include "ok/core/kernel.hpp"
#include "ok/core/test_point.hpp"
#include "ok/fs/ext4/layout.hpp"
#include "ok/fs/ext4/journal.hpp"
#include "ok/fs/ext4/txn.hpp"
#include "ok/block/sessions.hpp"
#include "ok/fs/ext4/mount.hpp"
#include "../points/file_attribute_checks.hpp"
#include "../points/directory_checks.hpp"
#include <new>

#include "../roadmap/roadmap_tests.hpp"

#include <array>
#include <span>

namespace ok
{
namespace
{

std::span<const std::byte> as_bytes(std::string_view text)
{
    return {reinterpret_cast<const std::byte *>(text.data()), text.size()};
}

bool contains_text(std::string_view haystack, std::string_view needle)
{
    if (needle.empty())
    {
        return true;
    }
    if (needle.size() > haystack.size())
    {
        return false;
    }
    for (usize offset = 0; offset <= haystack.size() - needle.size(); ++offset)
    {
        bool matched = true;
        for (usize i = 0; i < needle.size(); ++i)
        {
            if (haystack[offset + i] != needle[i])
            {
                matched = false;
                break;
            }
        }
        if (matched)
        {
            return true;
        }
    }
    return false;
}

void write_le16(std::span<std::byte> out, usize offset, u16 value)
{
    out[offset] = static_cast<std::byte>(value & 0xffu);
    out[offset + 1] = static_cast<std::byte>((value >> 8) & 0xffu);
}

void write_le32(std::span<std::byte> out, usize offset, u32 value)
{
    write_le16(out, offset, static_cast<u16>(value & 0xffffu));
    write_le16(out, offset + 2, static_cast<u16>((value >> 16) & 0xffffu));
}

uptr test_mapping_address(arch::Architecture architecture)
{
    switch (architecture)
    {
    case arch::Architecture::i386:
    case arch::Architecture::arm32:
    case arch::Architecture::rv32:
    case arch::Architecture::mips:
    case arch::Architecture::ppc:
        return static_cast<uptr>(0xc000'0000u);
    case arch::Architecture::x86_64:
    case arch::Architecture::aarch64:
    case arch::Architecture::rv64:
    case arch::Architecture::loongarch64:
    case arch::Architecture::mips64:
        return static_cast<uptr>(0xffff'8000'0000'0000ull);
    }
    return static_cast<uptr>(0xc000'0000u);
}

Status click_gui(Kernel &kernel, i32 x, i32 y)
{
    if (auto status = kernel.handle_gui_mouse_position(x, y, false); !status.ok())
    {
        return status;
    }
    if (auto status = kernel.handle_gui_mouse_position(x, y, true); !status.ok())
    {
        return status;
    }
    return kernel.handle_gui_mouse_position(x, y, false);
}

Status test_post_debug_suite_gui_input(Kernel &kernel)
{
    if (auto status = kernel.close_debug_gui(); !status.ok())
    {
        return status;
    }

    auto &compositor = kernel.gui().compositor();
    const auto shell_x = static_cast<i32>(6 + gui::taskbar_icon_size / 2);
    const auto shell_y =
        static_cast<i32>(driver::framebuffer_height - gui::taskbar_height + 3 + gui::taskbar_icon_size / 2);
    if (auto status = click_gui(kernel, shell_x, shell_y); !status.ok())
    {
        return status;
    }
    const auto shell_surface = kernel.debug_shell().gui_surface_id();
    auto shell_info = compositor.surface_info(shell_surface);
    if (shell_surface == 0 || !shell_info || !shell_info.value().focused ||
        shell_info.value().app != gui::TaskbarApp::debug_shell)
    {
        return Status::fault("post-debug mouse input did not launch a focused GUI shell");
    }

    if (auto status = kernel.handle_gui_key(ok_input_open_file_manager); !status.ok())
    {
        return status;
    }
    const auto file_surface = kernel.file_manager().surface_id();
    auto file_info = compositor.surface_info(file_surface);
    if (file_surface == 0 || !file_info || !file_info.value().focused ||
        file_info.value().app != gui::TaskbarApp::file_manager)
    {
        return Status::fault("post-debug keyboard shortcut did not launch a focused file manager");
    }

    if (auto status = kernel.close_file_manager(); !status.ok())
    {
        return status;
    }
    return kernel.debug_shell().close_all_gui();
}

} // namespace

Status Kernel::run_debug_test_suite()
{
    if (!booted_)
    {
        return Status::not_initialized("kernel not booted");
    }

    auto frame = memory_.frames().allocate();
    if (!frame)
    {
        return frame.status();
    }
    const auto test_address = test_mapping_address(config_.architecture);
    if (auto status = memory_.kernel_address_space().map(test_address, frame.value(), 0b11); !status.ok())
    {
        return status;
    }
    if (auto status = memory_.kernel_address_space().unmap(test_address); !status.ok())
    {
        return status;
    }
    if (auto status = memory_.frames().release(frame.value()); !status.ok())
    {
        return status;
    }
    test_report_.memory = true;

    arch::TrapFrame trap{.vector = 32, .context = arch_->make_kernel_context(0x1000, 0x8000)};
    if (auto status = interrupts_.dispatch(trap); !status.ok())
    {
        return status;
    }
    test_report_.interrupts = true;

    auto channel = ipc_.create_channel();
    if (!channel)
    {
        return channel.status();
    }
    struct Payload
    {
        u32 value;
    };
    if (auto status = ipc_.send_value(channel.value(), scheduler_.current_pid(), scheduler_.current_pid(), Payload{42});
        !status.ok())
    {
        return status;
    }
    auto message = ipc_.receive(channel.value());
    if (!message || message.value().size != sizeof(Payload))
    {
        return Status::fault("IPC debug test failed");
    }
    test_report_.ipc = true;

    if (auto status = vfs_.write_file("/tmp/kernel.log", as_bytes("booted\n")); !status.ok())
    {
        return status;
    }
    auto log = vfs_.read_file("/tmp/kernel.log");
    if (!log || log.value().size != 7)
    {
        return Status::fault("VFS debug test failed");
    }
    test_report_.vfs = true;

    const auto geometry = disk().geometry();
    if (geometry.block_count == 0 || geometry.block_size != driver::block_sector_size || !geometry.writable)
    {
        return Status::fault("block driver debug test failed");
    }
    static_cast<void>(simplefs_.unlink("/suite.txt"));
    if (auto status = simplefs_.create("/suite.txt", fs::NodeType::regular); !status.ok())
    {
        return status;
    }
    constexpr std::string_view simplefs_text{"simplefs"};
    if (auto status = simplefs_.write_file("/suite.txt", as_bytes(simplefs_text)); !status.ok())
    {
        return status;
    }
    auto simplefs_file = simplefs_.read_file("/suite.txt");
    if (!simplefs_file || simplefs_file.value().size != simplefs_text.size())
    {
        return Status::fault("SimpleFS read debug test failed");
    }
    auto simplefs_stat = simplefs_.stat("/suite.txt");
    if (!simplefs_stat || simplefs_stat.value().size != simplefs_text.size())
    {
        return Status::fault("SimpleFS stat debug test failed");
    }
    auto simplefs_listing = simplefs_.list_root();
    if (!simplefs_listing || simplefs_listing.value().count == 0)
    {
        return Status::fault("SimpleFS list debug test failed");
    }
    test_report_.simplefs = true;

    if (auto status = run_ext4_test(); !status.ok())
    {
        return status;
    }
    test_report_.ext4 = true;

    syscall::Request getpid{.number = syscall::Number::getpid, .caller = scheduler_.current_pid()};
    auto getpid_result = syscalls_.dispatch(getpid);
    if (!getpid_result.status.ok() || getpid_result.value != static_cast<i64>(scheduler_.current_pid()))
    {
        return Status::fault("getpid syscall debug test failed");
    }
    if (!syscalls_.has_handler(syscall::Number::openat) || !syscalls_.has_handler(syscall::Number::newfstatat) ||
        !syscalls_.has_handler(syscall::Number::mmap) || !syscalls_.has_handler(syscall::Number::arch_prctl))
    {
        return Status::fault("glibc baseline syscall handlers are missing");
    }
    test_report_.syscalls = true;

    auto context = arch_->make_user_context(arch::UserEntry{
        .instruction_pointer = 0x400000,
        .stack_pointer = 0x800000,
        .argument = 7,
    });
    if (auto status = user_space_.enter_process(scheduler_.current_pid(),
                                                arch::UserEntry{
                                                    .instruction_pointer = 0x400000,
                                                    .stack_pointer = 0x800000,
                                                    .argument = 7,
                                                },
                                                context);
        !status.ok())
    {
        return status;
    }
    if (context.mode != arch::PrivilegeMode::user)
    {
        return Status::fault("user mode transition debug test failed");
    }
    test_report_.user_mode = true;

    if (display_driver_.text().empty() || display_driver_.checksum() == 0)
    {
        return Status::fault("display debug test failed");
    }
    test_report_.display = true;
    if (!virtio_gpu_driver_.bound() || virtio_gpu_driver_.frames_presented() == 0)
    {
        return Status::fault("virtio GPU display debug test failed");
    }
    test_report_.gpu = true;
    if (gui_module_.compositor().state() != gui::GuiState::running ||
        gui_module_.compositor().last_present_checksum() == 0 ||
        kernel_modules_.services().query<gui::GuiCompositor>(gui::gui_service_id) != &gui_module_.compositor() ||
        kernel_modules_.services().query<gui::GuiDesktopService>(gui::gui_desktop_service_id) !=
            &gui_module_.desktop() ||
        kernel_modules_.kernel_process_pid() == 0 || scheduler_.find(kernel_modules_.kernel_process_pid()) == nullptr)
    {
        return Status::fault("kernel GUI debug test failed");
    }
    test_report_.gui = true;

    if (auto status = keyboard_driver_.feed_scancode(0x23); !status.ok())
    {
        return status;
    }
    auto key = keyboard_driver_.read_event();
    if (!key || !key.value().pressed || key.value().ascii != 'h')
    {
        return Status::fault("keyboard debug test failed");
    }
    if (auto status = mouse_driver_.feed_packet(driver::MousePacket{.delta_x = 3, .delta_y = -2, .left_button = true});
        !status.ok())
    {
        return status;
    }
    auto mouse = mouse_driver_.read_packet();
    if (!mouse || mouse.value().delta_x != 3 || mouse.value().delta_y != -2 || !mouse.value().left_button)
    {
        return Status::fault("mouse debug test failed");
    }
    test_report_.input = true;

    if (pci_bus_driver_.device_count() == 0 || pci_bus_driver_.find_class(0x0c, 0x03, 0x30) == nullptr ||
        pci_bus_driver_.find_class(0x01, 0x00, 0x00) == nullptr)
    {
        return Status::fault("PCIe debug test failed");
    }
    test_report_.bus = true;

    if (usb_xhci_driver_.find_device(driver::UsbDeviceClass::hid, 1, 1) == nullptr ||
        usb_xhci_driver_.find_device(driver::UsbDeviceClass::hid, 1, 2) == nullptr)
    {
        return Status::fault("USB enumeration debug test failed");
    }
    if (auto status = usb_keyboard_driver_.feed_report(driver::UsbKeyboardReport{.keys = {0x0b, 0, 0, 0, 0, 0}});
        !status.ok())
    {
        return status;
    }
    auto usb_key = usb_keyboard_driver_.read_event();
    if (!usb_key || usb_key.value().ascii != 'h')
    {
        return Status::fault("USB HID keyboard debug test failed");
    }
    if (auto status = usb_mouse_driver_.feed_report(
            driver::UsbMouseReport{.buttons = 1, .delta_x = 5, .delta_y = -4, .wheel = 0});
        !status.ok())
    {
        return status;
    }
    auto usb_mouse = usb_mouse_driver_.read_packet();
    if (!usb_mouse || usb_mouse.value().delta_x != 5 || usb_mouse.value().delta_y != -4 ||
        !usb_mouse.value().left_button)
    {
        return Status::fault("USB HID mouse debug test failed");
    }
    test_report_.usb = true;

    constexpr std::string_view udp_payload{"netdbg"};
    if (auto status = network_.send_udp(net::UdpEndpoint{.address = network_.local_address(), .port = 30000},
                                        net::UdpEndpoint{.address = network_.local_address(), .port = 30001},
                                        as_bytes(udp_payload));
        !status.ok())
    {
        return status;
    }
    auto datagram = network_.receive_udp();
    if (!datagram || datagram.value().payload_size != udp_payload.size())
    {
        return Status::fault("UDP/IP debug test failed");
    }
    if (auto status = network_.listen_tcp(31337); !status.ok())
    {
        return status;
    }
    auto tcp = network_.connect_tcp(net::UdpEndpoint{.address = network_.local_address(), .port = 31337}, 31338);
    if (!tcp || tcp.value().state != net::TcpState::established)
    {
        return Status::fault("TCP debug test failed");
    }
    test_report_.net = true;

    const auto original_mask = posix_.umask(01077);
    auto masked_fd = posix_.open("/tmp/mask-model", posix::o_CREAT | posix::o_EXCL | posix::o_RDWR, 0666);
    const auto stored_mask = posix_.umask(original_mask);
    if (!masked_fd || stored_mask != 0077)
        return Status::fault("common creation mask setup failed");
    auto masked_stat = posix_.fstat(masked_fd.value());
    if (!masked_stat || (masked_stat.value().mode & 0777) != 0600 ||
        !posix_.close(masked_fd.value()) || !posix_.unlink("/tmp/mask-model"))
        return Status::fault("common creation mask mode or lifetime failed");
    auto fd = posix_.open("/tmp/posix.txt", posix::o_CREAT | posix::o_RDWR | posix::o_TRUNC);
    if (!fd)
    {
        return fd.status();
    }
    constexpr std::string_view posix_text{"posix"};
    auto written = posix_.write(fd.value(), as_bytes(posix_text));
    if (!written || written.value() != posix_text.size())
    {
        return Status::fault("POSIX write debug test failed");
    }
    if (auto seek = posix_.seek(fd.value(), 0, posix::SeekWhence::set); !seek)
    {
        return seek.status();
    }
    std::array<std::byte, 8> posix_buffer{};
    auto read = posix_.read(fd.value(), posix_buffer);
    if (!read || read.value() != posix_text.size())
    {
        return Status::fault("POSIX read debug test failed");
    }
    auto posix_stat = posix_.fstat(fd.value());
    if (!posix_stat || posix_stat.value().size != posix_text.size())
    {
        return Status::fault("POSIX fstat debug test failed");
    }
    if (auto status = posix_.close(fd.value()); !status.ok())
    {
        return status;
    }
    if (auto status = posix_.chdir("/tmp"); !status.ok())
    {
        return status;
    }
    auto relative_fd =
        posix_.openat(posix::at_FDCWD, "posix-relative.txt", posix::o_CREAT | posix::o_RDWR | posix::o_TRUNC);
    if (!relative_fd)
    {
        return relative_fd.status();
    }
    if (auto status = posix_.close(relative_fd.value()); !status.ok())
    {
        return status;
    }
    if (auto relative_stat = posix_.statat(posix::at_FDCWD, "posix-relative.txt"); !relative_stat)
    {
        return Status::fault("POSIX relative path debug test failed");
    }
    auto dir_fd = posix_.open("/", posix::o_RDONLY | posix::o_DIRECTORY);
    if (!dir_fd)
    {
        return dir_fd.status();
    }
    std::array<std::byte, 256> dirents{};
    auto dirent_bytes = posix_.getdents64(dir_fd.value(), dirents);
    if (!dirent_bytes || dirent_bytes.value() == 0)
    {
        return Status::fault("POSIX getdents64 debug test failed");
    }
    if (auto status = posix_.close(dir_fd.value()); !status.ok())
    {
        return status;
    }
    auto mapping =
        posix_.mmap(0, 4096, posix::prot_READ | posix::prot_WRITE, posix::map_PRIVATE | posix::map_ANONYMOUS, -1, 0);
    if (!mapping)
    {
        return mapping.status();
    }
    if (auto status = posix_.mprotect(mapping.value(), 4096, posix::prot_READ); !status.ok())
    {
        return status;
    }
    if (auto status = posix_.munmap(mapping.value(), 4096); !status.ok())
    {
        return status;
    }
    if (auto status = posix_.chdir("/"); !status.ok())
    {
        return status;
    }
    test_report_.posix = true;

    auto shell_status = debug_shell_.execute("status");
    if (!shell_status || shell_status.value().empty())
    {
        return Status::fault("debug shell status test failed");
    }
    if (debug_shell_.gui_render_count() == 0 || debug_shell_.gui_surface_id() == 0)
    {
        return Status::fault("debug shell GUI render test failed");
    }
    auto shell_posix = debug_shell_.execute("posix");
    if (!shell_posix || shell_posix.value().empty())
    {
        return Status::fault("debug shell POSIX test failed");
    }
    auto shell_ls = debug_shell_.execute("ls /tmp");
    if (!shell_ls || shell_ls.value().empty())
    {
        return Status::fault("debug shell ls test failed");
    }
    auto shell_ls_flags = debug_shell_.execute("ls -a -lh /tmp");
    if (!shell_ls_flags || !contains_text(shell_ls_flags.value(), "./") ||
        !contains_text(shell_ls_flags.value(), "../") || !contains_text(shell_ls_flags.value(), "root root"))
    {
        return Status::fault("debug shell ls flags test failed");
    }
    auto shell_user = debug_shell_.execute("whoami");
    if (!shell_user || shell_user.value().empty())
    {
        return Status::fault("debug shell user test failed");
    }
    auto shell_su = debug_shell_.execute("su root");
    if (!shell_su || shell_su.value().empty())
    {
        return Status::fault("debug shell su test failed");
    }
    auto shell_users = debug_shell_.execute("users");
    if (!shell_users || contains_text(shell_users.value(), "kernel") || !contains_text(shell_users.value(), "root") ||
        !contains_text(shell_users.value(), "user"))
    {
        return Status::fault("debug shell users test failed");
    }
    static_cast<void>(debug_shell_.execute("touch /tmp/shell-owned"));
    auto shell_chown = debug_shell_.execute("chown user /tmp/shell-owned");
    if (!shell_chown || contains_text(shell_chown.value(), "only root"))
    {
        return Status::fault("debug shell chown was denied as non-root");
    }
    if (contains_text(shell_chown.value(), "shell error"))
    {
        return Status::fault("debug shell chown returned an error");
    }

    auto shell_chmod = debug_shell_.execute("chmod 600 /tmp/shell-owned");
    if (!shell_chmod || contains_text(shell_chmod.value(), "shell error"))
    {
        return Status::fault("debug shell chmod test failed");
    }

    auto shell_owned_stat = debug_shell_.execute("stat /tmp/shell-owned");
    if (!shell_owned_stat)
    {
        return Status::fault("debug shell owned stat command failed");
    }
    if (contains_text(shell_owned_stat.value(), "shell error"))
    {
        return Status::fault("debug shell owned stat returned an error");
    }
    if (!contains_text(shell_owned_stat.value(), "uid=1000"))
    {
        if (contains_text(shell_owned_stat.value(), "uid=0"))
        {
            return Status::fault("debug shell owned stat uid stayed root");
        }
        return Status::fault("debug shell owned stat uid test failed");
    }
    if (!contains_text(shell_owned_stat.value(), "mode=33152"))
    {
        return Status::fault("debug shell owned stat mode test failed");
    }

    auto shell_owned_ls = debug_shell_.execute("ls -lh /tmp");
    if (!shell_owned_ls || !contains_text(shell_owned_ls.value(), "user user"))
    {
        return Status::fault("debug shell owned ls user/group test failed");
    }
    auto shell_sfs_write = debug_shell_.execute("sfs write shell.txt hello");
    if (!shell_sfs_write)
    {
        return Status::fault("debug shell SimpleFS write test failed");
    }
    auto shell_sfs_ls = debug_shell_.execute("sfs ls");
    if (!shell_sfs_ls || shell_sfs_ls.value().empty())
    {
        return Status::fault("debug shell SimpleFS ls test failed");
    }
    auto shell_sh = debug_shell_.execute("false && echo bad || echo ok # comment");
    if (!shell_sh || shell_sh.value() != "ok\n")
    {
        return Status::fault("debug shell sh operator test failed");
    }
    auto shell_expand = debug_shell_.execute("export OKWORD=alpha; echo \"$OKWORD $PWD\"");
    if (!shell_expand || shell_expand.value() != "alpha /\n")
    {
        return Status::fault("debug shell quote and variable expansion test failed");
    }
    auto shell_pipe = debug_shell_.execute("echo alpha | grep alpha | wc -l");
    if (!shell_pipe || shell_pipe.value() != "1\n")
    {
        return Status::fault("debug shell pipeline test failed");
    }
    auto shell_append =
        debug_shell_.execute("echo first > /tmp/shell-pipe; echo second >> /tmp/shell-pipe; cat /tmp/shell-pipe");
    if (!shell_append || shell_append.value() != "first\nsecond\n")
    {
        return Status::fault("debug shell append redirection test failed");
    }
    auto shell_input_redirect = debug_shell_.execute("wc -l < /tmp/shell-pipe");
    if (!shell_input_redirect || shell_input_redirect.value() != "2\n")
    {
        return Status::fault("debug shell input redirection test failed");
    }
    auto shell_cp_mv = debug_shell_.execute(
        "cp /tmp/shell-pipe /tmp/shell-copy; mv /tmp/shell-copy /tmp/shell-moved; cat /tmp/shell-moved");
    if (!shell_cp_mv || shell_cp_mv.value() != "first\nsecond\n")
    {
        return Status::fault("debug shell cp/mv test failed");
    }
    auto shell_fm_tui = debug_shell_.execute("fm tui /tmp");
    if (!shell_fm_tui || !contains_text(shell_fm_tui.value(), "FILE MANAGER") ||
        !contains_text(shell_fm_tui.value(), "shell-pipe"))
    {
        return Status::fault("file manager TUI test failed");
    }
    auto shell_top_tui = debug_shell_.execute("top tui");
    if (!shell_top_tui || !contains_text(shell_top_tui.value(), "top -") ||
        !contains_text(shell_top_tui.value(), "%Cpu"))
    {
        return Status::fault("top TUI test failed");
    }
    auto shell_net_send = debug_shell_.execute("net udp 30002 shell-net");
    if (!shell_net_send)
    {
        return Status::fault("debug shell network send test failed");
    }
    auto shell_net_recv = debug_shell_.execute("net recv");
    if (!shell_net_recv || shell_net_recv.value().empty())
    {
        return Status::fault("debug shell network receive test failed");
    }
    auto shell_ext4 = debug_shell_.execute("ext4 status");
    if (!shell_ext4 || shell_ext4.value().empty())
    {
        return Status::fault("debug shell ext4 status test failed");
    }
    auto shell_system_tui = debug_shell_.execute("system tui");
    if (!shell_system_tui || !contains_text(shell_system_tui.value(), "kernel-console") ||
        !contains_text(shell_system_tui.value(), "mode=tui") || !contains_text(shell_system_tui.value(), "login=root"))
    {
        return Status::fault("debug shell system TUI test failed");
    }
    auto shell_su_kernel = debug_shell_.execute("su kernel");
    if (!shell_su_kernel || shell_su_kernel.value().empty())
    {
        return Status::fault("debug shell test failed");
    }
    auto shell_exit = debug_shell_.execute("exit");
    if (!shell_exit || shell_exit.value() != "root\n")
    {
        return Status::fault("debug shell exit user test failed");
    }
    shell_su_kernel = debug_shell_.execute("su kernel");
    if (!shell_su_kernel || shell_su_kernel.value() != "kernel\n")
    {
        return Status::fault("debug shell kernel user restore test failed");
    }
    shell_exit = debug_shell_.execute("exit");
    if (!shell_exit || shell_exit.value() != "root\n")
    {
        return Status::fault("debug shell nested exit test failed");
    }
    auto shell_close = debug_shell_.execute("exit");
    if (!shell_close || !shell_close.value().empty() || debug_shell_.gui_open())
    {
        return Status::fault("debug shell final exit did not close the GUI shell");
    }
    test_report_.shell = true;

    if (memory_.translation_mode() != config_.modes.memory || interrupts_.mode() != config_.modes.interrupts ||
        topology_.mode() != config_.modes.smp || scheduler_.mode() != config_.modes.scheduler ||
        ipc_.mode() != config_.modes.ipc || syscalls_.mode() != config_.modes.syscalls ||
        vfs_.mode() != config_.modes.filesystem || user_space_.mode() != config_.modes.user ||
        keyboard_driver_.mode() != config_.modes.drivers || mouse_driver_.mode() != config_.modes.drivers)
    {
        return Status::fault("module mode propagation failed");
    }
    test_report_.modes = true;

    if (auto status = run_module_roadmap_tests(test_report_); !status.ok())
    {
        return status;
    }
    if (auto status = run_vm_roadmap_tests(*this, test_report_); !status.ok())
    {
        return status;
    }
    if (auto status = run_process_roadmap_tests(*this, test_report_); !status.ok())
    {
        return status;
    }
    if (auto status = run_unix_vfs_roadmap_tests(*this, test_report_); !status.ok())
    {
        return status;
    }
    if (auto status = run_linux_abi_roadmap_tests(*this, test_report_); !status.ok())
    {
        return status;
    }
    if (auto status = run_driver_abi_roadmap_tests(test_report_); !status.ok())
    {
        return status;
    }
    if (auto status = run_gui_roadmap_tests(*this, test_report_); !status.ok())
    {
        return status;
    }
    if (auto status = run_network_storage_roadmap_tests(*this, test_report_); !status.ok())
    {
        return status;
    }
    if (auto status = run_smp_irq_preempt_roadmap_tests(*this, test_report_); !status.ok())
    {
        return status;
    }
    if (auto status = test_post_debug_suite_gui_input(*this); !status.ok())
    {
        return status;
    }

    auto debug_test_points = test::run_kernel_test_points(*this);
    if (!debug_test_points)
    {
        return debug_test_points.status();
    }
    debug_test_points_run_ = debug_test_points.value();

    if (auto status = close_debug_gui(); !status.ok())
    {
        return status;
    }
    if (debug_shell_.gui_open() || debug_shell_.gui_surface_id() != 0 || debug_shell_.process_id() != 0)
    {
        return Status::fault("debug GUI cleanup left a shell process behind");
    }
    for (const auto &process : scheduler_.processes())
    {
        if (process.name() == "oksh")
        {
            return Status::fault("debug GUI cleanup left an orphan oksh process behind");
        }
    }
    return Status::success();
}

Status Kernel::run_ext4_test()
{
    std::array<std::byte, 4096> image{};
    auto bytes = std::span<std::byte>(image.data(), image.size());
    const auto base = fs::ext4_superblock_offset;
    write_le32(bytes, base + 0x00, 128);
    write_le32(bytes, base + 0x04, 4);
    write_le32(bytes, base + 0x0c, 2);
    write_le32(bytes, base + 0x18, 0);
    write_le16(bytes, base + 0x38, fs::ext4_superblock_magic);
    write_le16(bytes, base + 0x58, 256);
    write_le32(bytes, base + 0x60, 0x40);

    constexpr std::string_view name{"OKEXT4"};
    for (usize i = 0; i < name.size(); ++i)
    {
        bytes[base + 0x78 + i] = static_cast<std::byte>(name[i]);
    }

    fs::Ext4Volume volume;
    if (auto status = volume.mount(image); !status.ok())
    {
        return status;
    }
    auto info = volume.info();
    if (!info || info.value().block_size != 1024 || info.value().inode_size != 256 ||
        info.value().volume_name.view() != name || !info.value().has_extents)
    {
        return Status::fault("EXT4 debug test failed");
    }

    std::array<std::byte, 1024> block{};
    if (auto status = volume.read_block(1, block); !status.ok())
    {
        return status;
    }

    auto &block_device = disk();
    if (auto status = block_device.write_blocks(0, std::span<const std::byte>(image.data(), image.size()));
        !status.ok())
    {
        return status;
    }
    fs::Ext4Volume block_volume;
    if (auto status = block_volume.mount(block_device); !status.ok())
    {
        return status;
    }
    if (auto status = block_volume.read_block(1, block); !status.ok())
    {
        return status;
    }
    // Boot-reached admission model; real-device recovery is a separate test.
    class RecoveryModelDevice final : public driver::BlockDevice
    {
      public:
        driver::BlockGeometry geometry() const override { return {4, 512, false}; }
        Status read_blocks(u64, std::span<std::byte> out) override
        {
            ++reads;
            for (auto &byte : out)
                byte = std::byte{0};
            return Status::success();
        }
        Status write_blocks(u64, std::span<const std::byte>) override
        {
            ++mutations;
            return Status::fault("recovery admission wrote its model device");
        }
        Status flush() override
        {
            ++mutations;
            return Status::fault("recovery admission flushed its model device");
        }
        usize reads{}, mutations{};
    } recovery_device;
    fs::Ext4RecoveryBinding binding;
    if (binding.inspect(recovery_device, {0, 4}).code() != StatusCode::unsupported || binding.info() ||
        recovery_device.reads != 1 || recovery_device.mutations ||
        binding.inspect(recovery_device, {0, 4}).code() != StatusCode::already_exists)
        return Status::fault("ext4 recovery binding rejection model failed");
    fs::Ext4ReplayPreview<> preview;
    if (preview.open(recovery_device, {0, 4}).code() != StatusCode::unsupported || preview.ready() ||
        preview.report().phase != fs::Ext4PreviewPhase::failed || recovery_device.reads != 2 ||
        recovery_device.mutations || preview.open(recovery_device, {0, 4}).code() != StatusCode::already_exists)
        return Status::fault("ext4 replay preview rejection model failed");
    alignas(fs::Ext4RecoveryCompletion<>) static std::array<std::byte, sizeof(fs::Ext4RecoveryCompletion<>)> completion_storage{};
    auto &completion = *::new (completion_storage.data()) fs::Ext4RecoveryCompletion<>;
    if (completion.run(recovery_device, {0, 4}).code() != StatusCode::denied || completion.report().complete() ||
        completion.report().failed_phase != fs::Ext4CompletionPhase::validating || recovery_device.reads != 2 ||
        recovery_device.mutations || completion.run(recovery_device, {0, 4}).code() != StatusCode::already_exists)
        return Status::fault("ext4 recovery completion rejection model failed");
    alignas(fs::Ext4MetadataTransaction) static std::array<std::byte, sizeof(fs::Ext4MetadataTransaction)> transaction_storage{};
    auto &transaction = *::new (transaction_storage.data()) fs::Ext4MetadataTransaction;
    if (transaction.commit(recovery_device, {0, 4}, {}).code() != StatusCode::denied || transaction.report().published() ||
        transaction.report().failed_phase != fs::Ext4TransactionPhase::validating || recovery_device.reads != 2 ||
        recovery_device.mutations || transaction.commit(recovery_device, {0, 4}, {}).code() != StatusCode::already_exists)
        return Status::fault("ext4 metadata transaction rejection model failed");
    block::BlockSessions<1> sessions;
    block::BlockSessions<1>::Session session, alias;
    const auto registration = sessions.register_device(recovery_device, 1);
    if (!registration || !sessions.acquire(registration.value(), {0, 4}, block::SessionAccess::readonly, session) ||
        sessions.acquire(registration.value(), {0, 4}, block::SessionAccess::readonly, alias).code() != StatusCode::busy ||
        session.flush().code() != StatusCode::denied || !session.close() ||
        !sessions.detach(registration.value()) || recovery_device.reads != 2 || recovery_device.mutations)
        return Status::fault("exclusive block session ownership model failed");
    using MetadataBuffers = fs::MetadataBuffers<1, 1024, 1>;
    alignas(MetadataBuffers) static std::array<std::byte, sizeof(MetadataBuffers)> metadata_storage{};
    auto &metadata = *::new (metadata_storage.data()) MetadataBuffers;
    const auto metadata_registration = sessions.register_device(recovery_device, 1);
    if (!metadata_registration || !metadata.open(sessions, metadata_registration.value(), {0, 4}, block::SessionAccess::readonly))
        return Status::fault("metadata buffer session model failed");
    {
        MetadataBuffers::Pin first, duplicate;
        if (!metadata.pin(0, first) || !metadata.pin(0, duplicate) ||
            first.report().value().read_pins != 2 || recovery_device.reads != 3 || recovery_device.mutations ||
            metadata.close().code() != StatusCode::busy || !first.release() || !duplicate.release())
            return Status::fault("canonical metadata buffer pin model failed");
    }
    if (!metadata.close() || !sessions.detach(metadata_registration.value()))
        return Status::fault("canonical metadata buffer retirement model failed");
    metadata.~MetadataBuffers();
    using OwnedTransaction = fs::Ext4OwnedTransaction<1>;
    alignas(OwnedTransaction) static std::array<std::byte, sizeof(OwnedTransaction)> owned_storage{};
    auto &owned = *::new (owned_storage.data()) OwnedTransaction;
    const auto owned_registration = sessions.register_device(recovery_device, 1);
    if (!owned_registration || owned.open(sessions, owned_registration.value(), {0, 4}).code() != StatusCode::denied ||
        owned.report().phase != fs::Ext4OwnedPhase::closed || !sessions.detach(owned_registration.value()) ||
        recovery_device.reads != 3 || recovery_device.mutations)
        return Status::fault("owned ext4 readonly admission model failed");
    const fs::Ext4CreateUpdate unsupported_create{2, 1700000111, 123456789, "empty",
        {fs::NodeType::regular, 0600, 1000, 1001}};
    if (owned.prepare_create(unsupported_create).code() != StatusCode::unsupported ||
        owned.report().phase != fs::Ext4OwnedPhase::closed || recovery_device.mutations)
        return Status::fault("owned create participant capacity rejection model failed");
    static fs::Ext4NextDataBlock next_image{};
    const fs::Ext4NextDataUpdate next_update{14, 1024, 1043, 1700000111, 123456789};
    if (owned.prepare_next_data(next_update, 1031, next_image).code() != StatusCode::unsupported ||
        owned.report().phase != fs::Ext4OwnedPhase::closed || recovery_device.mutations)
        return Status::fault("owned next data participant capacity rejection model failed");
    owned.~OwnedTransaction();
    static std::array<fs::Ext4CreateBlock, 6> create_sources{};
    static fs::Ext4CreatePlan create_plan{};
    const fs::Ext4CreateSource create_source{13, 0, 10, 11, 20,
        create_sources[0], create_sources[1], create_sources[2],
        create_sources[3], create_sources[4], create_sources[5]};
    static fs::Ext4RecoveryInfo empty_binding{};
    auto invalid_create = unsupported_create;
    invalid_create.nanoseconds = 1000000000;
    if (fs::Ext4CreatePlanner::plan(empty_binding, invalid_create, create_source, create_plan).code() !=
            StatusCode::invalid_argument || create_plan.count ||
        fs::Ext4CreatePlanner::plan(empty_binding, unsupported_create, create_source, create_plan).code() !=
            StatusCode::unsupported || create_plan.count || recovery_device.mutations)
        return Status::fault("empty create planner no-write rejection model failed");
    // Boot-reached planner/capacity rejection models; these do not execute
    // a data allocation or qualify transport and persistence ordering.
    static fs::Ext4NextDataPlan next_plan{};
    const fs::Ext4NextDataSource next_source{0, 10, 11,
        create_sources[0], create_sources[1], create_sources[2], create_sources[3]};
    auto invalid_next = next_update;
    invalid_next.old_size = 0;
    if (fs::Ext4NextDataPlanner::plan(empty_binding, invalid_next, next_source, next_plan).code() !=
            StatusCode::invalid_argument || next_plan.count ||
        fs::Ext4NextDataPlanner::plan(empty_binding, next_update, next_source, next_plan).code() !=
            StatusCode::unsupported || next_plan.count || recovery_device.mutations)
        return Status::fault("next data planner no-write rejection model failed");
    if (auto status = tests::check_file_attributes(); !status) return status;
    if (auto status = tests::check_directory_codec<abi::linux_user::configured_native_profile()>(); !status) return status;
    return simplefs_.format(block_device, "okroot");
}

} // namespace ok
