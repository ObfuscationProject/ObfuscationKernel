#pragma once
#include "ok/fs/archive.hpp"
#include "ok/core/shutdown.hpp"
#include "ok/sched/wait_registration.hpp"
namespace ok
{
class Kernel;
}
namespace ok::user::native
{
// Capture and qualify before kernel debug/native initialization changes mappings.
Status prepare_memory();
Status start(Kernel &kernel, fs::BootArchive archive);
bool step(Kernel &kernel);
// True when the last step found no thread to run: the kernel loop may idle
// until an interrupt (a tick, a device line) instead of stepping again.
bool idle();
Status runtime_status();
// Serialized native-owner proof. Historical generation counters are not owners.
struct NativeShutdownReport
{
    Status failure{};
    bool admission_closed{}, step_active{}, retirement_started{}, retirement_completed{};
    usize live_user_groups{}, native_threads{}, address_spaces{}, mapping_pages{};
    usize backend_spaces{}, active_entries{}, leased_entries{}, quarantined_entries{}, stranded_entries{};
    usize quarantined_spaces{}, quarantined_pins{}, wait_members{}, signal_owners{}, signal_waits{};
    usize origin_records{}, origin_borrows{}, origin_pending{}, vfs_records{}, vfs_pending{}, vfs_captures{};
    usize live_objects{}, object_pages{}, object_quarantine{};
    usize pool_pages{}, user_pages{}, pool_pins{}, pending_pages{}, quarantined_pages{};
    usize shared_files{}, shared_aliases{}, shared_retiring{}, shared_frames{}, shared_reservations{};
};
Result<ShutdownOwner> shutdown_owner(Kernel &kernel);
Result<ShutdownCaller> shutdown_caller(Kernel &kernel);
Status shutdown_preflight(Kernel &kernel, ShutdownOwner owner);
Status shutdown_check_commit(Kernel &kernel, ShutdownOwner owner);
Status shutdown_outer_boundary(Kernel &kernel);
Status shutdown_close_admission(Kernel &kernel);
Status shutdown_retire(Kernel &kernel);
NativeShutdownReport shutdown_report(Kernel &kernel);
Status shutdown_verify_drained(Kernel &kernel);
// `source` is the console's wake source (interrupt once its line is bound).
void input(char value, sched::WakeSource source = sched::WakeSource::thread);
void release_process(Kernel &kernel, u64 pid);
// OK_IRQ_PLATFORM and OK_IRQ_EVENTS, once the platform interrupt service runs.
void report_interrupts();
} // namespace ok::user::native
