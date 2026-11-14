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

namespace
{
// Marks this CPU's dispatch caller for its duration; nested dispatches restore.
class DispatchCaller final
{
  public:
    explicit DispatchCaller(sched::ProcessId caller) noexcept : cpu_(smp::current_cpu())
    {
        if (cpu_ >= dispatch_callers.size()) return;
        previous_ = dispatch_callers[cpu_];
        dispatch_callers[cpu_] = caller;
    }
    ~DispatchCaller()
    {
        if (cpu_ < dispatch_callers.size()) dispatch_callers[cpu_] = previous_;
    }
    DispatchCaller(const DispatchCaller &) = delete;
    DispatchCaller &operator=(const DispatchCaller &) = delete;

  private:
    smp::CpuId cpu_;
    sched::ProcessId previous_{};
};
} // namespace

Response Table::dispatch(const Request &request)
{
    // Handlers may register at runtime (load_module): never in a section.
    smp::check_exclusive("syscall::Table::dispatch");
    const DispatchCaller caller(request.caller);
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
