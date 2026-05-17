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
    queue_.push_back(message);
    return Status::success();
}

Result<Message> Channel::receive()
{
    if (queue_.empty()) {
        return Status::would_block("IPC channel empty");
    }
    auto message = queue_.front();
    queue_.pop_front();
    return message;
}

Result<ChannelId> IpcRouter::create_channel(usize capacity)
{
    if (capacity == 0) {
        return Status::invalid_argument("IPC channel capacity must be non-zero");
    }
    const auto id = next_channel_++;
    channels_.emplace(id, Channel {id, capacity});
    return id;
}

Status IpcRouter::send(ChannelId channel, Message message)
{
    auto it = channels_.find(channel);
    if (it == channels_.end()) {
        return Status::not_found("IPC channel not found");
    }
    return it->second.send(message);
}

Result<Message> IpcRouter::receive(ChannelId channel)
{
    auto it = channels_.find(channel);
    if (it == channels_.end()) {
        return Status::not_found("IPC channel not found");
    }
    return it->second.receive();
}

} // namespace ok::ipc

