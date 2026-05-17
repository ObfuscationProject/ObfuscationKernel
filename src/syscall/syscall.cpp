#include "ok/syscall/syscall.hpp"

namespace ok::syscall {

Status Table::register_handler(Number number, std::unique_ptr<Handler> handler)
{
    if (!handler) {
        return Status::invalid_argument("syscall handler is null");
    }
    const auto key = static_cast<u64>(number);
    if (handlers_.contains(key)) {
        return Status::already_exists("syscall handler already registered");
    }
    handlers_.emplace(key, std::move(handler));
    return Status::success();
}

Response Table::dispatch(const Request& request)
{
    const auto key = static_cast<u64>(request.number);
    auto it = handlers_.find(key);
    if (it == handlers_.end()) {
        return Response {.value = -1, .status = Status::unsupported("syscall not implemented")};
    }
    return it->second->invoke(request);
}

bool Table::has_handler(Number number) const
{
    return handlers_.contains(static_cast<u64>(number));
}

} // namespace ok::syscall

