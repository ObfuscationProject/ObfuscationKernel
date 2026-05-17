#pragma once

#include "ok/core/concepts.hpp"
#include "ok/core/types.hpp"
#include "ok/sched/scheduler.hpp"

#include <array>
#include <deque>
#include <unordered_map>

namespace ok::ipc {

using ChannelId = u64;
inline constexpr usize max_inline_payload = 128;

struct Message {
    sched::ProcessId sender {0};
    sched::ProcessId receiver {0};
    std::array<std::byte, max_inline_payload> payload {};
    usize size {0};
};

template <typename T>
concept MessagePayload = TriviallySerializable<T> && (sizeof(T) <= max_inline_payload);

class Channel final {
public:
    explicit Channel(ChannelId id, usize capacity = 32) : id_(id), capacity_(capacity) {}

    [[nodiscard]] ChannelId id() const { return id_; }
    Status send(Message message);
    Result<Message> receive();
    [[nodiscard]] usize queued() const { return queue_.size(); }

private:
    ChannelId id_;
    usize capacity_;
    std::deque<Message> queue_;
};

class IpcRouter final {
public:
    Result<ChannelId> create_channel(usize capacity = 32);
    Status send(ChannelId channel, Message message);
    Result<Message> receive(ChannelId channel);

    template <MessagePayload T>
    Status send_value(ChannelId channel, sched::ProcessId sender, sched::ProcessId receiver, const T& value)
    {
        Message message {};
        message.sender = sender;
        message.receiver = receiver;
        message.size = sizeof(T);
        const auto* bytes = reinterpret_cast<const std::byte*>(&value);
        for (usize i = 0; i < sizeof(T); ++i) {
            message.payload[i] = bytes[i];
        }
        return send(channel, message);
    }

private:
    std::unordered_map<ChannelId, Channel> channels_;
    ChannelId next_channel_ {1};
};

} // namespace ok::ipc

