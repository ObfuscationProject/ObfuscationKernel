#include "ok/ipc/ipc.hpp"

namespace ok::ipc {

Status Channel::send(Message message)
{
    if (message.size > max_inline_payload) {
        return Status::overflow("IPC inline payload too large");
    }
    if (queue_.size() >= capacity_) {
        return Status::would_block("IPC channel full");
    }
    return queue_.push(message);
}

Result<Message> Channel::receive()
{
    return queue_.pop();
}

Result<ChannelId> IpcRouter::create_channel(usize capacity)
{
    if (capacity == 0) {
        return Status::invalid_argument("IPC channel capacity must be non-zero");
    }
    if (capacity > max_channel_messages) {
        return Status::overflow("IPC channel capacity exceeds static queue size");
    }
    const auto id = next_channel_++;
    if (auto status = channels_.push_back(Channel {id, capacity}); !status.ok()) {
        return status;
    }
    return id;
}

Status IpcRouter::send(ChannelId channel, Message message)
{
    for (auto& item : channels_) {
        if (item.id() == channel) {
            return item.send(message);
        }
    }
    return Status::not_found("IPC channel not found");
}

Result<Message> IpcRouter::receive(ChannelId channel)
{
    for (auto& item : channels_) {
        if (item.id() == channel) {
            return item.receive();
        }
    }
    return Status::not_found("IPC channel not found");
}

} // namespace ok::ipc
