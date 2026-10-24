#pragma once
#include "ok/fs/storage.hpp"
namespace ok::test {
class PartialFailureDisk final : public driver::BlockDevice
{
  public:
    bool fail_read{}, fail_write{};
    std::byte first{};
    driver::BlockGeometry geometry() const override { return {16, 512, true}; }
    Status read_blocks(u64 sector, std::span<std::byte> out) override
    {
        if (out.size() != 512 || sector >= 16) return Status::invalid_argument();
        for (auto &value : out) value = sector ? std::byte(sector) : first;
        if (fail_read) { out[0] = std::byte{0xee}; return Status::io_error("partial cache test read"); }
        return {};
    }
    Status write_blocks(u64 sector, std::span<const std::byte> in) override
    {
        if (sector || in.size() != 512) return Status::invalid_argument();
        first = in[0];
        return fail_write ? Status::io_error("partial cache test write") : Status{};
    }
};
Status test_cache_failure()
{
    PartialFailureDisk disk;
    fs::BlockCache cache;
    if (auto status = cache.attach(disk); !status) return status;
    std::array<std::byte, 512> bytes{};
    for (u64 sector = 0; sector < fs::block_cache_entries; ++sector)
        if (auto status = cache.read_block(sector, bytes); !status) return status;
    disk.fail_read = true;
    if (cache.read_block(8, bytes).code() != StatusCode::io_error)
        return Status::fault("cache must preserve device read errors");
    disk.fail_read = false;
    if (auto status = cache.read_block(0, bytes); !status) return status;
    if (bytes[0] != std::byte{}) return Status::fault("failed read poisoned old cache identity");
    disk.fail_write = true;
    bytes[0] = std::byte{0x37};
    if (cache.write_block(0, bytes).code() != StatusCode::io_error)
        return Status::fault("cache must preserve device write errors");
    if (auto status = cache.read_block(0, bytes); !status) return status;
    return bytes[0] == std::byte{0x37} ? Status{} : Status::fault("failed write retained stale cached bytes");
}

}
