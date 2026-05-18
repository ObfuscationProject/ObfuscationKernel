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
    return handlers_.push_back(Entry{.number = key, .name = handler.name(), .handler = &handler});
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
    return handlers_.push_back(Entry{.number = key, .name = name, .context = context, .callback = callback});
}

Response Table::dispatch(const Request &request)
{
    const auto key = static_cast<u64>(request.number);
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
