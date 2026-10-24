#include "roadmap_tests.hpp"

#include "ok/fs/unix.hpp"

#include <array>
#include <span>

namespace ok
{
namespace
{

std::span<const std::byte> vfs_bytes(std::string_view text)
{
    return {reinterpret_cast<const std::byte *>(text.data()), text.size()};
}

bool vfs_buffer_equals(const fs::FileBuffer &buffer, std::string_view text)
{
    if (buffer.size != text.size())
    {
        return false;
    }
    for (usize i = 0; i < text.size(); ++i)
    {
        if (buffer.data[i] != static_cast<std::byte>(text[i]))
        {
            return false;
        }
    }
    return true;
}

bool vfs_bytes_equal(std::span<const std::byte> bytes, std::string_view text)
{
    if (bytes.size() != text.size())
    {
        return false;
    }
    for (usize i = 0; i < text.size(); ++i)
    {
        if (bytes[i] != static_cast<std::byte>(text[i]))
        {
            return false;
        }
    }
    return true;
}

bool vfs_contains(std::string_view haystack, std::string_view needle)
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

Status verify_regular_file(fs::VirtualFileSystem &vfs)
{
    static_cast<void>(vfs.unlink("/tmp/vfs-file"));
    if (auto status = vfs.create("/tmp/vfs-file", fs::NodeType::regular); !status.ok())
    {
        return status;
    }
    constexpr std::string_view text{"vfs file"};
    if (auto status = vfs.write_file("/tmp/vfs-file", vfs_bytes(text)); !status.ok())
    {
        return status;
    }
    auto read = vfs.read_file("/tmp/vfs-file");
    if (!read || !vfs_buffer_equals(read.value(), text))
    {
        return Status::fault("VFS regular file read/write validation failed");
    }
    auto metadata = vfs.stat("/tmp/vfs-file");
    if (!metadata || metadata.value().size != text.size() ||
        (metadata.value().mode & fs::mode_type_mask) != fs::mode_regular)
    {
        return Status::fault("VFS regular file metadata validation failed");
    }
    if (auto status = vfs.write_file("/tmp/vfs-file", std::span<const std::byte>{}); !status.ok())
    {
        return status;
    }
    auto truncated = vfs.stat("/tmp/vfs-file");
    return truncated && truncated.value().size == 0 ? Status::success()
                                                    : Status::fault("VFS regular file truncate validation failed");
}

Status verify_directories(Kernel &kernel)
{
    auto &posix = kernel.posix();
    static_cast<void>(posix.unlink("/tmp/vfs-nonempty/child"));
    static_cast<void>(posix.rmdir("/tmp/vfs-empty"));
    static_cast<void>(posix.rmdir("/tmp/vfs-nonempty"));

    const auto before = kernel.vfs().stat("/tmp");
    if (!before || before.value().type != fs::NodeType::directory)
    {
        return Status::fault("VFS /tmp metadata validation failed");
    }
    if (auto status = posix.mkdir("/tmp/vfs-empty"); !status.ok())
    {
        return status;
    }
    auto listing = kernel.vfs().list("/tmp");
    if (!listing || listing.value().count == 0)
    {
        return Status::fault("VFS directory listing validation failed");
    }
    auto after_mkdir = kernel.vfs().stat("/tmp");
    if (!after_mkdir || after_mkdir.value().link_count <= before.value().link_count)
    {
        return Status::fault("VFS directory link-count validation failed");
    }
    if (auto status = posix.rmdir("/tmp/vfs-empty"); !status.ok())
    {
        return status;
    }
    if (kernel.vfs().stat("/tmp/vfs-empty"))
    {
        return Status::fault("VFS empty directory removal left a visible node");
    }

    if (auto status = posix.mkdir("/tmp/vfs-nonempty"); !status.ok())
    {
        return status;
    }
    if (auto status = kernel.vfs().create("/tmp/vfs-nonempty/child", fs::NodeType::regular); !status.ok())
    {
        return status;
    }
    if (posix.rmdir("/tmp/vfs-nonempty").ok())
    {
        return Status::fault("VFS non-empty directory removal was not rejected");
    }
    if (auto status = posix.unlink("/tmp/vfs-nonempty/child"); !status.ok())
    {
        return status;
    }
    return posix.rmdir("/tmp/vfs-nonempty");
}

Status verify_initial_attributes(fs::VirtualFileSystem &vfs)
{
    // Kernel-resident namespace coverage, distinct from the musl syscall gate.
    if (auto status = vfs.create({}, "/tmp/vfs-group", fs::NodeType::directory, 02777, 19, 0x87654321); !status)
        return status;
    if (auto status = vfs.create({}, "/tmp/vfs-group/child", fs::NodeType::directory, 01750, 0xf1234567, 18); !status)
        return status;
    if (auto status = vfs.create({}, "/tmp/vfs-group/file", fs::NodeType::regular, 0640, 0xf1234567, 18); !status)
        return status;
    auto child = vfs.stat("/tmp/vfs-group/child"), file = vfs.stat("/tmp/vfs-group/file");
    if (!child || !file || child.value().mode != (fs::mode_directory | 03750) ||
        file.value().mode != (fs::mode_regular | 0640) || child.value().uid != 0xf1234567 ||
        file.value().uid != 0xf1234567 || child.value().gid != 0x87654321 || file.value().gid != 0x87654321)
        return Status::fault("VFS initial mode and ownership inheritance failed");
    if (auto status = vfs.unlink("/tmp/vfs-group/file"); !status)
        return status;
    if (auto status = vfs.rmdir("/tmp/vfs-group/child"); !status)
        return status;
    return vfs.rmdir("/tmp/vfs-group");
}

Status verify_paths_and_symlinks(fs::VirtualFileSystem &vfs)
{
    static_cast<void>(vfs.rmdir("/tmp/vfs-dot"));
    if (auto status = vfs.create("/tmp/vfs-dot", fs::NodeType::directory); !status.ok())
    {
        return status;
    }
    if (vfs.lookup("/tmp/.") != vfs.lookup("/tmp") || vfs.lookup("/tmp/vfs-dot/..") != vfs.lookup("/tmp"))
    {
        return Status::fault("VFS dot or dot-dot path resolution failed");
    }

    static_cast<void>(vfs.unlink("/tmp/vfs-link"));
    constexpr std::string_view target{"/tmp/vfs-file"};
    if (auto status = vfs.create({}, "/tmp/vfs-link", fs::NodeType::symlink, 0777, 0, 0, target); !status.ok())
        return status;
    auto link = vfs.resolve({}, "/tmp/vfs-link", fs::FinalComponent::no_follow);
    if (!link || link.value().node()->metadata().type != fs::NodeType::symlink)
        return Status::fault("VFS no-follow symlink validation failed");
    auto contents = link.value().node()->read(0, target.size());
    if (!contents || !vfs_buffer_equals(contents.value(), target) ||
        vfs.lookup("/tmp/vfs-link") != vfs.lookup(target))
        return Status::fault("VFS symlink traversal validation failed");
    return vfs.rmdir("/tmp/vfs-dot");
}

Status verify_file_offsets(Kernel &kernel)
{
    auto &posix = kernel.posix();
    static_cast<void>(posix.unlink("/tmp/vfs-offset"));
    auto first = posix.open("/tmp/vfs-offset", posix::o_CREAT | posix::o_RDWR | posix::o_TRUNC);
    if (!first)
    {
        return first.status();
    }
    constexpr std::string_view text{"abcdef"};
    auto written = posix.write(first.value(), vfs_bytes(text));
    if (!written || written.value() != text.size())
    {
        return Status::fault("VFS file offset setup write failed");
    }
    if (auto seek = posix.seek(first.value(), 0, posix::SeekWhence::set); !seek)
    {
        return seek.status();
    }
    auto second = posix.open("/tmp/vfs-offset", posix::o_RDONLY);
    if (!second)
    {
        static_cast<void>(posix.close(first.value()));
        return second.status();
    }

    std::array<std::byte, 3> first_read{};
    std::array<std::byte, 3> second_read{};
    auto first_count = posix.read(first.value(), std::span<std::byte>{first_read.data(), 2});
    auto second_count = posix.read(second.value(), second_read);
    static_cast<void>(posix.close(first.value()));
    static_cast<void>(posix.close(second.value()));
    if (!first_count || first_count.value() != 2 || !second_count || second_count.value() != 3 ||
        !vfs_bytes_equal(std::span<const std::byte>{second_read.data(), second_read.size()}, "abc"))
    {
        return Status::fault("VFS per-open file offset validation failed");
    }
    return Status::success();
}

Status verify_permissions(Kernel &kernel)
{
    auto &posix = kernel.posix();
    static_cast<void>(posix.set_identity(0, 0));
    static_cast<void>(posix.unlink("/tmp/vfs-private"));

    auto fd = posix.open("/tmp/vfs-private", posix::o_CREAT | posix::o_RDWR | posix::o_TRUNC, 0600);
    if (!fd)
    {
        return fd.status();
    }
    static_cast<void>(posix.close(fd.value()));

    auto root_stat = posix.stat("/tmp/vfs-private");
    if (!root_stat || (root_stat.value().mode & fs::mode_permission_mask) != 0600u ||
        root_stat.value().uid != 0 || root_stat.value().gid != 0)
    {
        return Status::fault("VFS create mode or ownership validation failed");
    }

    static_cast<void>(posix.set_identity(1000, 1000));
    if (posix.open("/tmp/vfs-private", posix::o_RDONLY).status().code() != StatusCode::denied ||
        posix.access("/tmp/vfs-private", posix::r_OK).code() != StatusCode::denied)
    {
        static_cast<void>(posix.set_identity(0, 0));
        return Status::fault("VFS permission denial validation failed");
    }

    static_cast<void>(posix.set_identity(0, 0));
    if (posix.access("/tmp/vfs-private", posix::x_OK).code() != StatusCode::denied)
        return Status::fault("VFS root access execute-bit validation failed");
    static_cast<void>(posix.set_credentials({.uid = 1000, .gid = 1000, .euid = 0, .egid = 0}));
    const bool access_ids_valid =
        posix.access("/tmp/vfs-private", posix::r_OK).code() == StatusCode::denied &&
        posix.faccessat(posix::at_FDCWD, "/tmp/vfs-private", posix::r_OK, posix::at_EACCESS) &&
        posix.faccessat(posix::at_FDCWD, "/tmp/vfs-private", posix::f_OK, 0x800u).code() == StatusCode::invalid_argument;
    static_cast<void>(posix.set_identity(0, 0));
    if (!access_ids_valid)
        return Status::fault("VFS access real/effective-ID and flag validation failed");
    if (auto status = posix.chmod("/tmp/vfs-private", 0644); !status.ok())
    {
        return status;
    }
    static_cast<void>(posix.set_identity(1000, 1000));
    auto readable = posix.open("/tmp/vfs-private", posix::o_RDONLY);
    if (!readable)
    {
        static_cast<void>(posix.set_identity(0, 0));
        return Status::fault("VFS chmod did not grant world read access");
    }
    static_cast<void>(posix.close(readable.value()));
    if (posix.open("/tmp/vfs-private", posix::o_WRONLY).status().code() != StatusCode::denied)
    {
        static_cast<void>(posix.set_identity(0, 0));
        return Status::fault("VFS chmod unexpectedly granted world write access");
    }

    static_cast<void>(posix.set_identity(0, 0));
    return posix.unlink("/tmp/vfs-private");
}

Status verify_naming_and_directory_identity(Kernel &kernel)
{
    auto &posix = kernel.posix();
    FixedString<96> previous_directory;
    if (auto status = previous_directory.assign(posix.getcwd()); !status.ok())
        return status;
    if (!posix.mkdir("/tmp/vfs-names") || !posix.mkdir("/tmp/vfs-names/left") ||
        !posix.mkdir("/tmp/vfs-names/right"))
        return Status::fault("VFS naming directory setup failed");
    auto left = posix.open("/tmp/vfs-names/left", posix::o_RDONLY | posix::o_DIRECTORY);
    auto right = posix.open("/tmp/vfs-names/right", posix::o_RDONLY | posix::o_DIRECTORY);
    if (!left || !right)
        return Status::fault("VFS naming directory descriptors failed");
    auto source = posix.openat(left.value(), "source", posix::o_CREAT | posix::o_RDWR);
    auto replaced = posix.openat(right.value(), "target", posix::o_CREAT | posix::o_RDWR);
    if (!source || !replaced || !posix.write(source.value(), vfs_bytes("new")) ||
        !posix.write(replaced.value(), vfs_bytes("old")))
        return Status::fault("VFS naming file setup failed");
    if (!posix.linkat(left.value(), "source", right.value(), "alias") ||
        !posix.renameat(left.value(), "source", right.value(), "target"))
        return Status::fault("VFS separate dirfd link or rename failed");
    auto source_stat = posix.fstat(source.value());
    auto replaced_stat = posix.fstat(replaced.value());
    std::array<std::byte, 3> content{};
    auto old_read = posix.pread(replaced.value(), content, 0);
    if (!source_stat || source_stat.value().link_count != 2 || !replaced_stat ||
        replaced_stat.value().link_count != 0 || !old_read || old_read.value() != 3 ||
        !vfs_bytes_equal(content, "old"))
        return Status::fault("VFS replacement lost an open inode");
    if (!posix.chdir("/tmp/vfs-names") || !posix.unlink("right/target") ||
        !posix.rename("right/./alias", "left/saved"))
        return Status::fault("VFS relative source and destination paths interfered");
    if (posix.link("left", "directory-link").code() != StatusCode::permission)
        return Status::fault("VFS directory hard link was not rejected");
    if (!posix.chdir("left/../left") || !posix.rename("/tmp/vfs-names/left", "/tmp/vfs-names/moved") ||
        posix.getcwd() != "/tmp/vfs-names/moved")
        return Status::fault("VFS directory rename lost the canonical cwd");
    auto via_directory = posix.openat(left.value(), "./saved", posix::o_RDONLY);
    if (!via_directory)
        return Status::fault("VFS renamed directory descriptor lost its inode");
    auto new_read = posix.read(via_directory.value(), content);
    if (!new_read || new_read.value() != 3 || !vfs_bytes_equal(content, "new") ||
        !posix.close(via_directory.value()))
        return Status::fault("VFS renamed directory descriptor read failed");
    if (!posix.chmod("/tmp/vfs-names/moved", 0777) || !posix.chmod("/tmp/vfs-names", 0000) ||
        !posix.set_identity(1000, 1000))
        return Status::fault("VFS search permission setup failed");
    const bool search_checked =
        posix.link("/tmp/vfs-names/moved/saved", "/tmp/vfs-search-escape").code() == StatusCode::denied &&
        posix.rename("/tmp/vfs-names/moved/saved", "/tmp/vfs-search-escape").code() == StatusCode::denied &&
        posix.link("/tmp/vfs-file", "/tmp/vfs-names/moved/hidden").code() == StatusCode::denied &&
        posix.linkat(left.value(), "saved", left.value(), "search-copy").ok() &&
        posix.rename("search-copy", "search-moved").ok() && posix.unlink("search-moved").ok();
    static_cast<void>(posix.set_identity(0, 0));
    static_cast<void>(posix.chmod("/tmp/vfs-names", 0755));
    static_cast<void>(posix.chmod("/tmp/vfs-names/moved", 0755));
    if (!search_checked)
        return Status::fault("VFS name operations bypassed search permissions or rejected a held directory");
    if (!posix.link("saved", "alias") || !posix.rename("saved", "alias") ||
        !posix.stat("saved") || !posix.stat("alias") || !posix.unlink("saved") || !posix.unlink("alias"))
        return Status::fault("VFS same-inode rename or unlink failed");
    source_stat = posix.fstat(source.value());
    new_read = posix.pread(source.value(), content, 0);
    if (!source_stat || source_stat.value().link_count != 0 || !new_read || new_read.value() != 3 ||
        !vfs_bytes_equal(content, "new"))
        return Status::fault("VFS final unlink invalidated an open description");
    if (!posix.close(source.value()) || !posix.close(replaced.value()) ||
        !posix.close(left.value()) || !posix.close(right.value()) || !posix.chdir(previous_directory.view()) ||
        !posix.rmdir("/tmp/vfs-names/moved") || !posix.rmdir("/tmp/vfs-names/right"))
        return Status::fault("VFS naming cleanup failed");
    return posix.rmdir("/tmp/vfs-names");
}

Status verify_device_nodes()
{
    fs::DeviceNode null_device{fs::DeviceKind::null};
    fs::DeviceNode zero_device{fs::DeviceKind::zero};
    fs::DeviceNode console_device{fs::DeviceKind::console};

    constexpr std::string_view text{"device"};
    auto null_write = null_device.write(vfs_bytes(text));
    if (!null_write || null_write.value() != text.size() || null_device.bytes_written() != text.size())
    {
        return Status::fault("VFS /dev/null write validation failed");
    }
    std::array<std::byte, 8> zeros{};
    auto zero_read = zero_device.read(zeros);
    if (!zero_read || zero_read.value() != zeros.size())
    {
        return Status::fault("VFS /dev/zero read validation failed");
    }
    for (auto byte : zeros)
    {
        if (byte != std::byte{0})
        {
            return Status::fault("VFS /dev/zero returned non-zero data");
        }
    }
    auto console_write = console_device.write(vfs_bytes(text));
    if (!console_write || console_write.value() != text.size() || console_device.bytes_written() != text.size())
    {
        return Status::fault("VFS /dev/console write validation failed");
    }
    return Status::success();
}

Status verify_pipe_and_tty()
{
    fs::Pipe pipe;
    auto initial_poll = pipe.poll();
    if (!initial_poll || (initial_poll.value() & 0x04u) == 0 || (initial_poll.value() & 0x01u) != 0)
    {
        return Status::fault("VFS empty pipe poll validation failed");
    }
    constexpr std::string_view payload{"pipe"};
    auto written = pipe.write(vfs_bytes(payload));
    auto readable_poll = pipe.poll();
    if (!written || written.value() != payload.size() || !readable_poll || (readable_poll.value() & 0x01u) == 0)
    {
        return Status::fault("VFS pipe transfer poll validation failed");
    }
    std::array<std::byte, 8> out{};
    auto read = pipe.read(out);
    if (!read || read.value() != payload.size() ||
        !vfs_bytes_equal(std::span<const std::byte>{out.data(), payload.size()}, payload))
    {
        return Status::fault("VFS pipe byte transfer validation failed");
    }
    pipe.set_non_blocking(true);
    auto empty = pipe.read(out);
    if (empty || empty.status().code() != StatusCode::would_block)
    {
        return Status::fault("VFS non-blocking empty pipe validation failed");
    }

    fs::TtyDevice tty;
    auto tty_write = tty.write(vfs_bytes("tty"));
    auto tcgets = tty.ioctl(0x5401, 0);
    auto tcsets = tty.ioctl(0x5402, 0);
    auto tcgets_after = tty.ioctl(0x5401, 0);
    if (!tty_write || tty_write.value() != 3 || !tcgets || tcgets.value() != 1 || !tcsets || !tcgets_after ||
        tcgets_after.value() != 0 || tty.echo())
    {
        return Status::fault("VFS TTY console or ioctl validation failed");
    }
    return Status::success();
}

Status verify_shell_commands(Kernel &kernel)
{
    static_cast<void>(kernel.posix().unlink("/tmp/a"));
    static_cast<void>(kernel.posix().rmdir("/tmp/d"));

    auto root = kernel.debug_shell().execute("ls /");
    if (!root || !vfs_contains(root.value(), "dev/") || !vfs_contains(root.value(), "tmp/"))
    {
        return Status::fault("VFS shell ls / validation failed");
    }
    auto dev = kernel.debug_shell().execute("ls /dev");
    if (!dev || !vfs_contains(dev.value(), "null") || !vfs_contains(dev.value(), "zero") ||
        !vfs_contains(dev.value(), "console") || !vfs_contains(dev.value(), "tty0"))
    {
        return Status::fault("VFS shell ls /dev validation failed");
    }
    auto zero = kernel.debug_shell().execute("cat /dev/zero");
    if (!zero)
    {
        return Status::fault("VFS shell cat /dev/zero validation failed");
    }
    if (!kernel.debug_shell().execute("touch /tmp/a"))
    {
        return Status::fault("VFS shell touch validation failed");
    }
    if (!kernel.debug_shell().execute("echo hello > /tmp/a"))
    {
        return Status::fault("VFS shell output redirection validation failed");
    }
    auto cat = kernel.debug_shell().execute("cat /tmp/a");
    if (!cat || cat.value() != "hello\n")
    {
        return Status::fault("VFS shell cat /tmp/a validation failed");
    }
    if (!kernel.debug_shell().execute("mkdir /tmp/d"))
    {
        return Status::fault("VFS shell mkdir validation failed");
    }
    if (!kernel.debug_shell().execute("rm /tmp/a"))
    {
        return Status::fault("VFS shell rm validation failed");
    }
    if (kernel.vfs().stat("/tmp/a"))
    {
        return Status::fault("VFS shell rm left file visible");
    }
    return kernel.posix().rmdir("/tmp/d");
}

} // namespace

Status run_unix_vfs_roadmap_tests(Kernel &kernel, KernelTestReport &report)
{
    fs::UnixVfsModel model;
    if (auto status = model.initialize(kernel.vfs()); !status.ok())
    {
        return status;
    }
    if (auto status = model.validate_mounts(); !status.ok())
    {
        return status;
    }
    if (auto status = verify_regular_file(kernel.vfs()); !status.ok())
    {
        return status;
    }
    if (auto status = verify_directories(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = verify_initial_attributes(kernel.vfs()); !status.ok())
    {
        return status;
    }
    if (auto status = verify_paths_and_symlinks(kernel.vfs()); !status.ok())
    {
        return status;
    }
    if (auto status = verify_file_offsets(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = verify_permissions(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = verify_naming_and_directory_identity(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = verify_device_nodes(); !status.ok())
    {
        return status;
    }
    if (auto status = verify_pipe_and_tty(); !status.ok())
    {
        return status;
    }
    if (auto status = verify_shell_commands(kernel); !status.ok())
    {
        return status;
    }

    report.vfs_unix = true;
    report.devfs = true;
    report.pipe = true;
    report.tty = true;
    return Status::success();
}

} // namespace ok
