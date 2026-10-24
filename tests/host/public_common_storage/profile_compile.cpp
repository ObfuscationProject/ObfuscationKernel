#include "ok/fs/ext4_mounted_common_profile.hpp"
#include <new>

using Mount = ok::fs::CommonProfileMount;

extern "C" Mount *ok_common_profile_construct(void *memory, Mount::CommonStorage &storage)
{
    return ::new (memory) Mount{storage};
}

extern "C" ok::Status ok_common_profile_open(Mount &mount, Mount::Registry &registry,
                                            Mount::Registry::Registration key,
                                            ok::block::SessionBounds bounds)
{
    return mount.mount(registry, key, bounds);
}
