#include "ok/fs/ext4/mount.hpp"

using Registry = ok::block::BlockSessions<1>;
extern "C" ok::Status ok_direct_read_scope_probe(Registry::Session &session,
    ok::block::SessionReport &shadow, ok::u64 first, std::span<std::byte> bytes)
{
    auto authority = []() -> ok::Status { return {}; };
    return ok::fs::guarded_ext4_direct_read(session, shadow, authority, first, bytes);
}
