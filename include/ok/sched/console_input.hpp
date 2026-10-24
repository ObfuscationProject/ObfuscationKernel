#pragma once

#include "ok/sched/console_input_profile.hpp"
#include <array>
#include <span>

namespace ok::sched
{
// The owning scheduler authenticates canonical callers and mints PCB membership.
// All operations run on the serialized boot CPU; this is not an IRQ/SMP queue.
class ConsoleInputLease final
{
  public:
    ConsoleInputLease() = default;
    ConsoleInputLease(const ConsoleInputLease &) = delete;
    ConsoleInputLease &operator=(const ConsoleInputLease &) = delete;

    Status bind_manager(u64 pid)
    {
        if (manager_ || pid != 1) return Status::denied("console manager must be the original init");
        manager_ = pid;
        manager_alive_ = true;
        return {};
    }

    Result<u32> claim(u64 caller, u64 target)
    {
        if (!manager(caller) || !target || target == manager_)
            return Status::denied("console claim requires init and a distinct target");
        if (active_) return Status::busy("console input already has an owner");
        if (epoch_ == native_console_epoch_limit)
            return Status::overflow("console lease generations exhausted");
        erase();
        ++epoch_;
        leader_ = target;
        active_ = true;
        return epoch_;
    }

    Status revoke(u64 caller, u32 expected)
    {
        if (!manager(caller)) return Status::denied("console revoke requires init");
        if (!expected || expected != epoch_) return Status::denied("console revoke generation is stale");
        close();
        return {};
    }

    Status flush(u64 caller, u32 membership, u32 expected)
    {
        if (!active_ || !expected || expected != epoch_ ||
            !(manager(caller) || (caller == leader_ && authorized(caller, membership, expected))))
            return Status::denied("console flush requires its current leader or init");
        erase();
        return {};
    }

    // Identity retirement never grants a lease to adopted descendants.
    bool retire(u64 pid)
    {
        if (!pid) return false;
        const bool affected = active_ && (pid == leader_ || pid == manager_);
        if (pid == manager_) manager_alive_ = false;
        if (affected) close();
        return affected;
    }

    [[nodiscard]] bool manager(u64 pid) const { return manager_alive_ && pid && pid == manager_; }
    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] u32 epoch() const { return epoch_; }
    [[nodiscard]] u64 leader() const { return leader_; }
    [[nodiscard]] usize buffered() const { return size_; }
    [[nodiscard]] bool authorized(u64 pid, u32 membership, u32 expected = 0) const
    {
        return pid && active_ && membership == epoch_ && (!expected || expected == epoch_);
    }
    [[nodiscard]] u32 inherit(u32 membership) const
    {
        return active_ && membership == epoch_ ? epoch_ : 0;
    }
    [[nodiscard]] bool readable(u64 pid, u32 membership, u32 expected) const
    {
        return authorized(pid, membership, expected) && size_;
    }

    bool input(char value)
    {
        if (!active_ || size_ == buffer_.size()) return false;
        buffer_[(head_ + size_) % buffer_.size()] = static_cast<std::byte>(value);
        ++size_;
        return true;
    }

    Result<usize> read(u64 pid, u32 membership, u32 expected, std::span<std::byte> output)
    {
        if (!authorized(pid, membership, expected)) return Status::denied("console lease is not readable by caller");
        if (output.empty()) return usize{0};
        if (!size_) return Status::would_block("console input queue is empty");
        const auto count = output.size() < size_ ? output.size() : size_;
        for (usize index = 0; index < count; ++index)
        {
            output[index] = buffer_[head_];
            wipe(buffer_[head_]);
            head_ = (head_ + 1) % buffer_.size();
        }
        size_ -= count;
        return count;
    }

  private:
    static void wipe(std::byte &value) { *static_cast<volatile std::byte *>(&value) = std::byte{}; }
    void erase()
    {
        for (auto &byte : buffer_) wipe(byte);
        head_ = size_ = 0;
    }
    void close() { active_ = false; leader_ = 0; erase(); }
    friend struct ConsoleInputLeaseTestAccess;
    u64 manager_{}, leader_{};
    u32 epoch_{};
    bool manager_alive_{}, active_{};
    std::array<std::byte, 256> buffer_{};
    usize head_{}, size_{};
};
} // namespace ok::sched
