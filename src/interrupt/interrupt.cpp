#include "ok/interrupt/interrupt.hpp"

namespace ok::interrupt {

Status InterruptDispatcher::register_handler(Vector vector, std::unique_ptr<InterruptHandler> handler)
{
    if (vector >= max_vectors || !handler) {
        return Status::invalid_argument("invalid interrupt handler");
    }
    if (handlers_[vector]) {
        return Status::already_exists("interrupt vector already registered");
    }
    handlers_[vector] = std::move(handler);
    return Status::success();
}

Status InterruptDispatcher::dispatch(arch::TrapFrame& frame)
{
    if (frame.vector >= max_vectors) {
        return Status::invalid_argument("interrupt vector out of range");
    }
    auto& handler = handlers_[frame.vector];
    if (!handler) {
        return Status::not_found("unhandled interrupt vector");
    }
    auto status = handler->handle(frame);
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
    return vector < max_vectors && handlers_[vector] != nullptr;
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

