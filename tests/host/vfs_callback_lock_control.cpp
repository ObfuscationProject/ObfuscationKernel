// The frozen predecessor must reject this callback-lock assertion at mount.
#include "ok/fs/vfs.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
using namespace ok;
struct Probe final : fs::Node
{
    fs::VirtualFileSystem &vfs;
    explicit Probe(fs::VirtualFileSystem &vfs) : vfs(vfs) {}
    std::string_view name() const override { return "/"; }
    fs::Metadata metadata() const override
    {
        if (!vfs.lock_.try_lock())
        { std::fprintf(stderr, "provider callback held global VFS spinlock\n"); std::abort(); }
        vfs.lock_.unlock();
        return {.type = fs::NodeType::directory, .mode = fs::mode_for(fs::NodeType::directory, 0777), .link_count = 1};
    }
    Result<fs::FileBuffer> read(usize, usize) const override { return fs::FileBuffer{}; }
    Status write(usize, std::span<const std::byte>) override { return {}; }
    fs::Node *lookup(std::string_view) override { return nullptr; }
    Status create(std::string_view, fs::NodeType) override { return Status::unsupported(); }
    Status chmod(u32) override { return {}; }
    Status chown(u32, u32) override { return {}; }
};
int main()
{
    auto vfs = std::make_unique<fs::VirtualFileSystem>(); Probe root(*vfs);
    if (!vfs->mount("/", root)) return 1;
    std::puts("PASS provider callback global lock control");
}
