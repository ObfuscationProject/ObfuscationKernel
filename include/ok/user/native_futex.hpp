#pragma once

#include "ok/posix/posix.hpp"
#include <bit>

namespace ok
{
class Kernel;
}
namespace ok::user::native
{
enum class FutexOperation : u8
{
    wait,
    wake
};
enum class FutexKeyDomain : u8
{
    private_address_space,
    anonymous_shared_operation
};
struct FutexRequest
{
    FutexOperation operation{FutexOperation::wait};
    uptr address{};
    u32 value{};
    posix::ClockTime timeout{};
    bool timed{};
    bool big_endian{std::endian::native == std::endian::big};
    FutexKeyDomain domain{FutexKeyDomain::private_address_space};
};
struct FutexWakeOperationRequest
{
    uptr first_address{}, second_address{};
    u32 first_limit{}, second_limit{}, encoded_operation{};
    bool big_endian{std::endian::native == std::endian::big};
    FutexKeyDomain domain{FutexKeyDomain::private_address_space};
};
struct FutexRequeueRequest
{
    uptr source_address{}, destination_address{};
    u32 wake_limit{}, requeue_limit{}, expected{};
    bool compare{};
    bool big_endian{std::endian::native == std::endian::big};
    FutexKeyDomain domain{FutexKeyDomain::private_address_space};
};
// Private keys contain domain, live non-reused PID and virtual word address.
// Ordinary operations on private backing use a separate process-local domain.
// Ordinary operations on shared anonymous backing use the allocator owner,
// frame index, generation and aligned word offset, retained by a waiter pin.
// File mappings and cross-process CLONE_VM need a separate backing-key service.
// Calls run on any CPU. Waits queue on 64 hash buckets, each with its own
// lock: WAIT compares the user word with one atomic load and queues under
// its bucket; WAKE completes waiters in queue order and counts only those it
// woke (a waiter that already timed out or was interrupted is skipped). The
// waiter's own resume produces its result.
i64 futex(Kernel &kernel, sched::ProcessId pid, const FutexRequest &request, bool &complete);
// One atomic read-modify-write of the second word, then conditional wakes,
// holding the caller's address-space lock and both buckets. This operation
// registers no wait.
i64 futex_wake_operation(Kernel &kernel, sched::ProcessId pid, const FutexWakeOperationRequest &request);
// Non-PI requeue retains the original WAIT continuation. CMP_REQUEUE compares
// with one atomic load holding the caller's address-space lock and both
// buckets; waiters keep FIFO order within a bucket. PI and RT priority are
// gaps.
i64 futex_requeue(Kernel &kernel, sched::ProcessId pid, const FutexRequeueRequest &request);
inline i64 private_futex(Kernel &kernel, sched::ProcessId pid, const FutexRequest &request, bool &complete)
{
    return request.domain == FutexKeyDomain::private_address_space ? futex(kernel, pid, request, complete) : -38;
}
} // namespace ok::user::native
