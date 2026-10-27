#include "ok/fs/ext4/txn.hpp"

ok::Status compile_ext4_preview(ok::driver::BlockDevice &device, ok::fs::Ext4ReplayPreview<> &preview,
                                 ok::fs::Ext4RecoveryBounds bounds, std::span<ok::u8> bytes)
{
    if (const auto status = preview.open(device, bounds); !status)
        return status;
    const auto number = preview.lookup(2, "seed");
    if (!number)
        return number.status();
    if (const auto inode = preview.inode(number.value()); !inode)
        return inode.status();
    if (const auto read = preview.read(number.value(), 0, bytes); !read)
        return read.status();
    return preview.directory(2, [](const ok::fs::Ext4ReadOnlyDirent &) { return ok::Status::success(); });
}
