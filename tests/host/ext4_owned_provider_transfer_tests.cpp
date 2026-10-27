// A caller withholds a returned owned Node while another host thread unlinks it.
// This covers the interval after backend admission ends and before handoff to
// the receiving caller, independently of the transport-pause ownership tests.
#include "ext4_realtime_model.hpp"
#include "ok/fs/ext4/layout.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace ok;
using namespace ok::fs;
#define CHECK(...) do { if (!(__VA_ARGS__)) { \
    std::fprintf(stderr, "legacy transfer:%d: %s\n", __LINE__, #__VA_ARGS__); std::abort(); \
} } while (false)
using Bytes = std::vector<std::byte>;
namespace
{
class Disk final : public driver::BlockDevice
{
  public:
    explicit Disk(const Bytes &input) : bytes(input) {}
    driver::BlockGeometry geometry() const override { unlocked(); return {bytes.size() / 512, 512, true}; }
    Status read_blocks(u64 first, std::span<std::byte> output) override
    {
        unlocked(); CHECK(first * 512 + output.size() <= bytes.size());
        std::memcpy(output.data(), bytes.data() + first * 512, output.size()); return {};
    }
    Status write_blocks(u64 first, std::span<const std::byte> input) override
    {
        unlocked(); CHECK(first * 512 + input.size() <= bytes.size());
        std::memcpy(bytes.data() + first * 512, input.data(), input.size()); return {};
    }
    Status flush() override { unlocked(); return {}; }
    Ext4FileSystem *owner{};
    Bytes bytes;
  private:
    void unlocked() const
    { if (owner) { CHECK(owner->identity_lock_.try_lock()); owner->identity_lock_.unlock(); } }
};
class TransferGate
{
  public:
    void returned_and_wait()
    {
        std::unique_lock guard(lock); returned = true; changed.notify_all();
        CHECK(changed.wait_for(guard, std::chrono::seconds(10), [&] { return receive; }));
    }
    void wait_returned()
    {
        std::unique_lock guard(lock);
        CHECK(changed.wait_for(guard, std::chrono::seconds(10), [&] { return returned; }));
    }
    void accept() { std::lock_guard guard(lock); receive = true; changed.notify_all(); }
  private:
    std::mutex lock;
    std::condition_variable changed;
    bool returned{}, receive{};
};
void transfer(const Bytes &input, bool create, bool via_vfs)
{
    Disk disk(input); auto storage = std::make_unique<Ext4FileSystem>(); auto &fs = *storage;
    CHECK(fs.mount(disk)); disk.owner = &fs; auto *root = fs.root(); CHECK(root);
    auto vfs = std::make_unique<VirtualFileSystem>(); CHECK(vfs->mount("/", *root));
    if (!create) CHECK(root->create("handoff", NodeType::regular));
    TransferGate gate;
    Result<Node *> received{Status::not_found()};
    std::optional<PathReference> received_path;
    std::thread provider([&] {
        if (via_vfs)
        {
            auto local = create ? vfs->create_file({}, "/handoff", 0644, 0, 0, false)
                                : vfs->resolve({}, "/handoff");
            CHECK(local); gate.returned_and_wait(); received_path.emplace(std::move(local.value()));
        }
        else
        {
            auto local = create ? root->create_child_retained("handoff", {NodeType::regular, 0644, 0, 0})
                                : root->lookup_child_retained("handoff");
            CHECK(local); gate.returned_and_wait(); received = local;
        }
    });
    gate.wait_returned();
    // The operation has returned but its result is still held by the producer.
    // Actual namespace removal therefore races the in-flight caller handoff.
    CHECK(!fs.operation_active_); CHECK(root->remove("handoff", false));
    CHECK(root->lookup_child_retained("handoff").status().code() == StatusCode::not_found);
    gate.accept(); provider.join();
    auto *node = static_cast<Ext4Node *>(via_vfs ? received_path->node() : received.value());
    CHECK(node && node->phase_ == Ext4Node::Phase::detached && node->references_ == 1);
    CHECK(!node->borrowed_identity_exposed_);
    auto metadata = node->query_metadata(); CHECK(metadata && !metadata.value().link_count);
    const auto inode = metadata.value().attributes.inode;
    const std::byte payload[]{std::byte{'h'}, std::byte{'e'}, std::byte{'l'}, std::byte{'d'}};
    CHECK(node->write(0, payload)); auto content = node->read(0, 4);
    CHECK(content && content.value().size == 4 && std::memcmp(content.value().data.data(), payload, 4) == 0);
    CHECK(via_vfs ? received_path->release() : node->release());
    CHECK(node->phase_ == Ext4Node::Phase::vacant && !node->references_);
    auto replacement = root->create_child_retained("replacement", {NodeType::regular, 0644, 0, 0});
    CHECK(replacement && replacement.value() == node);
    CHECK(replacement.value()->query_metadata().value().attributes.inode == inode);
    CHECK(replacement.value()->release()); CHECK(fs.sync());
}
}
int main(int argc, char **argv)
{
    CHECK(argc == 2); testing::ext4_clock::reset();
    auto *file = std::fopen(argv[1], "rb"); CHECK(file); CHECK(!std::fseek(file, 0, SEEK_END));
    Bytes input(static_cast<usize>(std::ftell(file))); CHECK(!std::fseek(file, 0, SEEK_SET));
    CHECK(std::fread(input.data(), 1, input.size(), file) == input.size()); std::fclose(file);
    for (bool create : {false, true}) for (bool via_vfs : {false, true}) transfer(input, create, via_vfs);
    CHECK(testing::ext4_clock::calls() == 0);
    std::puts("PASS actual legacy provider returned-hold transfer: producer_pauses=4 concurrent_unlinks=4 provider_and_vfs_lookup_create=4 held_data_survives=4 scoped_slot_reuses=4 memory_lock_spans_io=0");
}
