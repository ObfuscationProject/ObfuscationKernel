#define main frozen_continuation_main
#include "native_vfs_stat_continuation_tests.cpp"
#undef main
#include "../../src/user/native_shared_capture.hpp"

namespace ok::arch::execution
{
bool user_range(const AddressSpace &, uptr address, usize bytes, bool)
{ return address >= user_begin && address < user_end && bytes <= user_end - address; }
Status copy_from_user(const AddressSpace &, void *, uptr, usize)
{ return Status::fault("this synthetic statx capture has no executable provider mapping"); }
}
static_assert(!std::is_default_constructible_v<native::BackendCapture::Observation>);
static_assert(!std::is_copy_constructible_v<native::BackendCapture::Observation>);
static_assert(!std::is_move_assignable_v<native::BackendCapture::Observation>);
static usize shared_cases, capability_cases;
template <lu::Profile P> void shared_statx()
{
    Fixture<P> f;
    using Origins = native::SyscallOriginTable<P>;
    auto origins = std::make_unique<Origins>(f.pool);
    CHECK(origins->prepare(f.thread()));
    native::SharedNativeCapture<P> capture(f.pool);
    f.thread().native_frame = f.frame();
    auto view = f.pool.activate_entry(f.thread().entry_stack, {f.pid, f.pid}, 0); CHECK(view);
    const auto before = hosted_entries;
    const auto trap = capture.enter(*origins, *f.adapter, *f.scheduler->find(f.pid), f.thread(), view.value(), *f.scheduler);
    CHECK(trap && hosted_entries == before + 1);
    CHECK(f.pool.deactivate_entry(f.thread().entry_stack, {f.pid, f.pid}, 0));
    f.thread().syscall_pending = true;
    CHECK(!origins->live(origins->identity(*f.scheduler->find(f.pid), f.thread())));
    Pause paused(*f.vfs, f.provider); bool complete = true;
    CHECK(f.begin(complete) == -11 && !complete && f.reads == 1);
    const auto pending = f.adapter->pending(f.pid, f.pid);
    CHECK(f.user_path.assign("/tmp/changed-after-shared-capture")); f.user_output += 128;
    paused.release(); CHECK(f.adapter->drain(*f.scheduler, *f.vfs)); complete = true;
    CHECK(f.resume(pending, complete) == 0 && complete && f.reads == 1 && f.copies == 1);
    CHECK(f.output_address == hw::user_begin + 1024 && hosted_entries == before + 1);
    ++shared_cases;
}
void capability_guards()
{
    using F = Fixture<lu::Profile::x86_64>;
    using Origins = native::SyscallOriginTable<lu::Profile::x86_64>;
    for (unsigned mode = 0; mode < 6; ++mode)
    {
        F f; auto origins = std::make_unique<Origins>(f.pool); CHECK(origins->prepare(f.thread()));
        f.thread().native_frame = f.frame();
        auto view = f.pool.activate_entry(f.thread().entry_stack, {f.pid, f.pid}, 0); CHECK(view);
        auto origin_stage = origins->stage_capture(*f.scheduler->find(f.pid), f.thread(), view.value(), *f.scheduler); CHECK(origin_stage);
        auto vfs_stage = f.adapter->stage_capture(*f.scheduler->find(f.pid), f.thread(), view.value(), *f.scheduler); CHECK(vfs_stage);
        native::BackendCapture backend(f.pool); const auto before = hosted_entries;
        auto observation = backend.enter(*f.scheduler, *f.scheduler->find(f.pid), f.thread(), view.value()); CHECK(observation);
        CHECK(hosted_entries == before + 1);
        if (mode == 0)
        {
            CHECK(!backend.enter(*f.scheduler, *f.scheduler->find(f.pid), f.thread(), view.value()));
            CHECK(hosted_entries == before + 1);
        }
        if (mode == 1)
        {
            CHECK(origins->observe(origin_stage.value(), observation.value(), *f.scheduler->find(f.pid), f.thread(), view.value(), *f.scheduler));
            CHECK(!origins->observe(origin_stage.value(), observation.value(), *f.scheduler->find(f.pid), f.thread(), view.value(), *f.scheduler));
            CHECK(!native::BackendCapture::claim(observation.value(), native::BackendCapture::Observer::restart_origin,
                *f.scheduler, *f.scheduler->find(f.pid), f.thread(), view.value()));
        }
        if (mode == 2)
        {
            auto moved = std::move(observation.value()); CHECK(observation.value().sequence() == 0);
            CHECK(!f.adapter->observe(vfs_stage.value(), observation.value(), *f.scheduler->find(f.pid), f.thread(), view.value(), *f.scheduler));
            CHECK(f.adapter->observe(vfs_stage.value(), moved, *f.scheduler->find(f.pid), f.thread(), view.value(), *f.scheduler));
        }
        if (mode == 3)
        {
            ++f.thread().native_frame.fp_state[13];
            CHECK(!f.adapter->observe(vfs_stage.value(), observation.value(), *f.scheduler->find(f.pid), f.thread(), view.value(), *f.scheduler));
            --f.thread().native_frame.fp_state[13];
            CHECK(f.adapter->observe(vfs_stage.value(), observation.value(), *f.scheduler->find(f.pid), f.thread(), view.value(), *f.scheduler));
        }
        if (mode == 4)
        {
            auto copied = f.thread();
            CHECK(!f.adapter->observe(vfs_stage.value(), observation.value(), *f.scheduler->find(f.pid), copied, view.value(), *f.scheduler));
        }
        CHECK(f.pool.deactivate_entry(f.thread().entry_stack, {f.pid, f.pid}, 0));
        CHECK(!native::BackendCapture::claim(observation.value(), native::BackendCapture::Observer::vfs_continuation,
            *f.scheduler, *f.scheduler->find(f.pid), f.thread(), view.value()));
        ++capability_cases;
    }
    {
        F f; native::BackendCapture backend(f.pool);
        auto view = f.pool.activate_entry(f.thread().entry_stack, {f.pid, f.pid}, 0); CHECK(view);
        mock::invalid_stack = true; const auto before = hosted_entries;
        CHECK(!backend.enter(*f.scheduler, *f.scheduler->find(f.pid), f.thread(), view.value())); mock::invalid_stack = false;
        CHECK(!backend.enter(*f.scheduler, *f.scheduler->find(f.pid), f.thread(), view.value())); CHECK(hosted_entries == before + 1);
        CHECK(f.pool.deactivate_entry(f.thread().entry_stack, {f.pid, f.pid}, 0)); ++capability_cases;
    }
}
int main()
{
    shared_statx<lu::Profile::i386>(); shared_statx<lu::Profile::x86_64>();
    shared_statx<lu::Profile::aarch64>(); shared_statx<lu::Profile::arm32>();
    shared_statx<lu::Profile::rv64>(); shared_statx<lu::Profile::rv32>();
    shared_statx<lu::Profile::loongarch64>(); shared_statx<lu::Profile::mips>();
    shared_statx<lu::Profile::mips64>(); shared_statx<lu::Profile::ppc>(); capability_guards();
    std::printf("PASS shared owned capture checks=%zu profiles=%zu capability_guards=%zu hosted_backend_entries=%zu duplicate_backend_calls=0 guest=0 signal_statx_restart=0\n",
        checks, shared_cases, capability_cases, hosted_entries);
}
