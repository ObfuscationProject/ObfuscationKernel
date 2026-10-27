#pragma once

#include "ok/fs/ext4/mount.hpp"

namespace owned_fixture
{
template <typename Mount>
ok::Result<ok::fs::VfsMountHandle> publish_owned_root(Mount &mount, ok::fs::VirtualFileSystem &vfs,
                                                    std::string_view path = "/")
{
    auto reference = mount.retain_root();
    if (!reference) return reference.status();
    return vfs.mount_owned(path, std::move(reference).value());
}
template <typename Mount>
ok::Status detach_and_drain(Mount &mount, ok::fs::VirtualFileSystem &vfs, ok::fs::VfsMountHandle handle)
{
    if (auto status = vfs.detach(handle); !status) return status;
    return mount.begin_drain();
}
} // namespace owned_fixture
