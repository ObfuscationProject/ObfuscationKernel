#include "ok/interrupt/interrupt.hpp"

namespace ok::interrupt {

Status InterruptDispatcher::register_handler(Vector vector, InterruptHandler& handler)
{
    if (vector >= max_vectors) {
        return Status::invalid_argument("invalid interrupt handler");
    }
    if (handlers_[vector].handler != nullptr || handlers_[vector].callback != nullptr) {
        return Status::already_exists("interrupt vector already registered");
    }
    handlers_[vector] = Entry {.name = handler.name(), .handler = &handler};
    return Status::success();
}

Status InterruptDispatcher::register_callback(Vector vector, std::string_view name, void* context, Callback callback)
{
    if (vector >= max_vectors || callback == nullptr) {
        return Status::invalid_argument("invalid interrupt callback");
    }
    if (handlers_[vector].handler != nullptr || handlers_[vector].callback != nullptr) {
        return Status::already_exists("interrupt vector already registered");
    }
    handlers_[vector] = Entry {.name = name, .context = context, .callback = callback};
    return Status::success();
}

Status InterruptDispatcher::dispatch(arch::TrapFrame& frame)
{
    if (frame.vector >= max_vectors) {
        return Status::invalid_argument("interrupt vector out of range");
    }
    auto& entry = handlers_[frame.vector];
    if (entry.handler == nullptr && entry.callback == nullptr) {
        return Status::not_found("unhandled interrupt vector");
    }
    auto status = entry.handler != nullptr ? entry.handler->handle(frame) : entry.callback(entry.context, frame);
    if (status.ok()) {
        ++handled_counts_[frame.vector];
    }
    return status;
}

usize InterruptDispatcher::handled_count(Vector vector) const
{
    if (vector >= max_vectors) {
        return 0;
    }
    return handled_counts_[vector];
}

bool InterruptDispatcher::has_handler(Vector vector) const
{
    return vector < max_vectors && (handlers_[vector].handler != nullptr || handlers_[vector].callback != nullptr);
}

Status SimulatedInterruptController::initialize()
{
    initialized_ = true;
    return Status::success();
}

Status SimulatedInterruptController::enable(Vector vector)
{
    if (!initialized_) {
        return Status::not_initialized("interrupt controller not initialized");
    }
    if (vector >= max_vectors) {
        return Status::invalid_argument("interrupt vector out of range");
    }
    enabled_[vector] = true;
    return Status::success();
}

Status SimulatedInterruptController::disable(Vector vector)
{
    if (!initialized_) {
        return Status::not_initialized("interrupt controller not initialized");
    }
    if (vector >= max_vectors) {
        return Status::invalid_argument("interrupt vector out of range");
    }
    enabled_[vector] = false;
    return Status::success();
}

bool SimulatedInterruptController::enabled(Vector vector) const
{
    return vector < max_vectors && enabled_[vector];
}

} // namespace ok::interrupt
