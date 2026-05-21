#include "ok/syscall/syscall.hpp"

namespace ok::syscall
{

Status Table::register_handler(Number number, Handler &handler)
{
    const auto key = static_cast<u64>(number);
    for (const auto &entry : handlers_)
    {
        if (entry.number == key)
        {
            return Status::already_exists("syscall handler already registered");
        }
    }
    if (auto status = handlers_.push_back(Entry{.number = key, .name = handler.name(), .handler = &handler});
        !status.ok())
    {
        return status;
    }
    if (key < direct_handlers_.size())
    {
        direct_handlers_[static_cast<usize>(key)] = &handlers_[handlers_.size() - 1];
    }
    return Status::success();
}

Status Table::register_callback(Number number, std::string_view name, void *context, Callback callback)
{
    if (callback == nullptr)
    {
        return Status::invalid_argument("syscall callback is null");
    }
    const auto key = static_cast<u64>(number);
    for (const auto &entry : handlers_)
    {
        if (entry.number == key)
        {
            return Status::already_exists("syscall handler already registered");
        }
    }
    if (auto status =
            handlers_.push_back(Entry{.number = key, .name = name, .context = context, .callback = callback});
        !status.ok())
    {
        return status;
    }
    if (key < direct_handlers_.size())
    {
        direct_handlers_[static_cast<usize>(key)] = &handlers_[handlers_.size() - 1];
    }
    return Status::success();
}

Response Table::dispatch(const Request &request)
{
    const auto key = static_cast<u64>(request.number);
    if (key < direct_handlers_.size())
    {
        if (auto *entry = direct_handlers_[static_cast<usize>(key)]; entry != nullptr)
        {
            if (entry->handler != nullptr)
            {
                return entry->handler->invoke(request);
            }
            return entry->callback(entry->context, request);
        }
    }
    for (auto &entry : handlers_)
    {
        if (entry.number != key)
        {
            continue;
        }
        if (entry.handler != nullptr)
        {
            return entry.handler->invoke(request);
        }
        return entry.callback(entry.context, request);
    }
    return Response{.value = -1, .status = Status::unsupported("syscall not implemented")};
}

bool Table::has_handler(Number number) const
{
    const auto key = static_cast<u64>(number);
    if (key < direct_handlers_.size())
    {
        return direct_handlers_[static_cast<usize>(key)] != nullptr;
    }
    for (const auto &entry : handlers_)
    {
        if (entry.number == key)
        {
            return true;
        }
    }
    return false;
}

} // namespace ok::syscall
