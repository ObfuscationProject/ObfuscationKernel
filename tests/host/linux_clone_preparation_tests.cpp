#include "ok/user/linux_clone.hpp"

#include <cstdio>
#include <cstdlib>

using namespace ok;
namespace hw = arch::execution;
namespace lu = abi::linux_user;
namespace clone_abi = user::linux_bootstrap;
using Pool = memory::KernelEntryStackPool;
static usize checks, stores;
static bool reject_store;
static std::array<u8, 4> parent_tid;
alignas(Pool::page_size) static std::array<std::byte, Pool::arena_bytes> arena;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"%d: %s\n",__LINE__,#x); std::abort(); } } while(false)

namespace ok::arch::execution
{
Status copy_from_user(const AddressSpace &, void *, uptr, usize)
{
    return Status::fault("this clone test does not read user pointers");
}
Status copy_to_user(const AddressSpace &, uptr address, const void *source, usize size)
{
    ++stores;
    if (reject_store || address != user_begin + 128 || size != 4)
        return Status::fault("injected parent TID store failure");
    const auto *bytes = static_cast<const u8 *>(source);
    for (usize i = 0; i < 4; ++i) parent_tid[i] = bytes[i];
    return Status::success();
}
}

template <lu::Profile Profile> void profile(Kernel &kernel)
{
    Pool pool; CHECK(pool.activate(arena));
    const auto context = kernel.arch().make_user_context({hw::user_begin, hw::user_begin + 4096});
    auto result = kernel.scheduler().create_user_process("clone-prepare", context); CHECK(result);
    const auto pid = result.value(); auto *process = kernel.scheduler().find(pid);
    CHECK(kernel.scheduler().set_runnable(pid)); CHECK(kernel.scheduler().activate_thread(pid, pid));
    hw::UserFrame frame{}; frame.pc = hw::user_begin;
    lu::NativeBridge<Profile>::set_stack_pointer(frame, hw::user_begin + 4096);
    frame.fp_state[5] = 0x9a;
    lu::Call call{};
    call.arguments[0] = clone_abi::clone_flags::shared_group | clone_abi::clone_flags::parent_settid;
    call.arguments[1] = hw::user_begin + 8192;
    call.arguments[2] = hw::user_begin + 128;
    const usize before = process->threads().size();
    stores = 0; reject_store = false; parent_tid = {0x11, 0x22, 0x33, 0x44};
    usize preparations{};
    auto denied = clone_abi::clone_thread<Profile>(kernel, pid, frame, call, clone_abi::RejectThreadPreparation{});
    CHECK(denied == -syscall::ErrnoMapper::errno_for(Status::unsupported()));
    CHECK(process->threads().size() == before && stores == 0);
    auto failure = clone_abi::clone_thread<Profile>(kernel, pid, frame, call,
        [&](sched::ThreadControlBlock &child) -> Status {
            ++preparations;
            CHECK(child.owner == pid && child.state == sched::ProcessState::created);
            CHECK(!kernel.scheduler().find_thread(pid, child.tid) && stores == 0);
            auto lease = pool.reserve({pid, child.tid}); CHECK(lease);
            CHECK(pool.release(lease.value(), {pid, child.tid}));
            return Status::no_memory("injected failure after cleaned preparation");
        });
    CHECK(failure == -12 && preparations == 1 && stores == 0);
    CHECK(process->threads().size() == before && pool.accounting().leased_slots == 0);
    CHECK(parent_tid[0] == 0x11 && parent_tid[3] == 0x44);
    auto prepare = [&](sched::ThreadControlBlock &child) -> Status {
        ++preparations;
        CHECK(!kernel.scheduler().find_thread(pid, child.tid));
        CHECK(child.state == sched::ProcessState::created && child.native_active);
        CHECK(child.native_frame.fp_state[5] == 0x9a);
        auto lease = pool.reserve({pid, child.tid}); if (!lease) return lease.status();
        auto top = pool.top(lease.value(), {pid, child.tid}); CHECK(top);
        child.entry_stack = lease.value(); child.kernel_stack_top = top.value();
        return Status::success();
    };
    auto child_id = clone_abi::clone_thread<Profile>(kernel, pid, frame, call, prepare);
    CHECK(child_id > 0 && preparations == 2 && stores == 1);
    auto *child = kernel.scheduler().find_thread(pid, static_cast<sched::ThreadId>(child_id));
    CHECK(child && child->entry_stack.pool == &pool && child->kernel_stack_top != 0);
    CHECK(child->state == sched::ProcessState::runnable && child->native_active);
    auto returned = lu::NativeBridge<Profile>::decode_return(child->native_frame);
    CHECK(returned && returned.value().value == 0 && !returned.value().error);
    u32 decoded{};
    for (usize i = 0; i < 4; ++i)
        decoded |= static_cast<u32>(parent_tid[i]) << (8 * (lu::Codec<Profile>::profile.big_endian ? 3 - i : i));
    CHECK(decoded == child_id);
    CHECK(pool.release(child->entry_stack, {pid, child->tid})); child->entry_stack = {}; child->kernel_stack_top = 0;
    CHECK(kernel.scheduler().exit_thread(pid, child->tid));
    reject_store = true;
    child_id = clone_abi::clone_thread<Profile>(kernel, pid, frame, call, prepare);
    CHECK(child_id > 0 && stores == 2); // Linux ignores post-publication parent-store failure.
    child = kernel.scheduler().find_thread(pid, static_cast<sched::ThreadId>(child_id)); CHECK(child);
    CHECK(pool.release(child->entry_stack, {pid, child->tid})); child->entry_stack = {}; child->kernel_stack_top = 0;
    CHECK(kernel.scheduler().exit_thread(pid, child->tid));
    CHECK(pool.accounting().leased_slots == 0);
    CHECK(kernel.scheduler().exit_process(pid, 0)); CHECK(kernel.scheduler().reap_process(pid));
}

int main()
{
    static Kernel kernel;
    profile<lu::Profile::i386>(kernel); profile<lu::Profile::x86_64>(kernel);
    profile<lu::Profile::aarch64>(kernel); profile<lu::Profile::arm32>(kernel);
    profile<lu::Profile::rv64>(kernel); profile<lu::Profile::rv32>(kernel);
    profile<lu::Profile::loongarch64>(kernel); profile<lu::Profile::mips>(kernel);
    profile<lu::Profile::mips64>(kernel); profile<lu::Profile::ppc>(kernel);
    std::printf("PASS native clone preparation profiles=10 checks=%zu guest=0 hardware_isolation=0 smp=0\n", checks);
}
