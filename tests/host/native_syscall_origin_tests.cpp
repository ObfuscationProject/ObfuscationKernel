// Actual entry wrapper, runtime dispatch and scheduler with hosted trap/memory.
#define OK_ENABLE_TEST_POINTS 1
#define OK_SYSCALL_ORIGIN_MODEL 1
#include "native_clock_model.hpp"
#include <source_location>
namespace ok::user::native
{
struct SyscallOriginTestAccess
{
    static u64 capture(SyscallOrigins &table, u64 next)
    { const auto previous = table.capture_sequence_; table.capture_sequence_ = next; return previous; }
    static u64 image(SyscallOrigins &table, u64 next)
    { const auto previous = table.image_sequence_; table.image_sequence_ = next; return previous; }
};
}
namespace
{
using Origins = native::SyscallOrigins;
constexpr auto profile = lu::configured_native_profile();
constexpr uptr output = hw::user_begin + 128;
usize checks{};
void expect(bool value, std::source_location where = std::source_location::current())
{
    ++checks;
    if (!value) { std::fprintf(stderr, "SYSCALL_ORIGIN_FAIL line=%u checks=%zu\n", where.line(), checks); std::abort(); }
}
struct Group { sched::ProcessId pid{}; sched::ThreadId reader{}; };
Group create(Kernel &kernel)
{
    auto made = kernel.scheduler().create_user_process("syscall-origin",
        kernel.arch().make_user_context({hw::user_begin, hw::user_begin + 8192})); CHECK(made);
    auto *p = kernel.scheduler().find(made.value());
    p->resources().personality = sched::UserPersonality::linux;
    p->resources().native_space = {0x100000 + made.value() * 4096, static_cast<u32>(made.value() % 16), 1};
    CHECK(native::allocate(p->resources().native_pages, hw::user_begin, hw::page_read | hw::page_execute));
    CHECK(native::allocate(p->resources().native_pages, hw::user_begin + 4096, hw::page_read | hw::page_execute));
    auto peer = kernel.scheduler().create_thread(p->pid(), p->threads()[0].context); CHECK(peer);
    for (auto &thread : p->threads()) { CHECK(native::prepare_native_thread(thread)); thread.native_active = true; }
    p->threads()[0].native_active = false;
    CHECK(kernel.scheduler().set_runnable(p->pid()));
    CHECK(kernel.scheduler().activate_thread(p->pid(), p->pid())); CHECK(kernel.posix().activate(p->pid()));
    native::runtime_kernel = &kernel; native::started = true;
    return {p->pid(), peer.value()};
}
void leader(Kernel &kernel, Group g)
{ CHECK(kernel.scheduler().activate_thread(g.pid, g.pid)); CHECK(kernel.posix().activate(g.pid)); }
void retire(Kernel &kernel, Group g)
{
    auto *p = kernel.scheduler().find(g.pid);
    if (!p) return;
    native::release_process(kernel, g.pid);
    CHECK(kernel.scheduler().exit_process(g.pid, 0)); CHECK(kernel.scheduler().reap_orphans());
    expect(native::pipe_read_objects.accounting().live_objects == 0);
    expect(native::runtime_status().ok());
}
hw::Trap syscall_trap()
{
    if constexpr (profile == lu::Profile::x86_64) return {hw::TrapKind::syscall, 256};
    if constexpr (profile == lu::Profile::i386) return {hw::TrapKind::syscall, 128};
    if constexpr (profile == lu::Profile::aarch64) return {hw::TrapKind::syscall, 0x15, 0x56000000};
    if constexpr (profile == lu::Profile::arm32) return {hw::TrapKind::syscall, 2};
    if constexpr (profile == lu::Profile::rv32 || profile == lu::Profile::rv64) return {hw::TrapKind::syscall, 8, 8};
    if constexpr (profile == lu::Profile::loongarch64) return {hw::TrapKind::syscall, 11};
    if constexpr (profile == lu::Profile::mips || profile == lu::Profile::mips64) return {hw::TrapKind::syscall, 8, 32};
    return {hw::TrapKind::syscall, 8};
}
usize instruction_bytes(bool thumb = false)
{ return profile == lu::Profile::i386 || profile == lu::Profile::x86_64 || (profile == lu::Profile::arm32 && thumb) ? 2 : 4; }
void opcode(uptr site, bool thumb)
{
    std::array<u8, 4> code{};
    if constexpr (profile == lu::Profile::x86_64) code = {0x0f, 0x05};
    if constexpr (profile == lu::Profile::i386) code = {0xcd, 0x80};
    if constexpr (profile == lu::Profile::aarch64) code = {0x01, 0x00, 0x00, 0xd4};
    if constexpr (profile == lu::Profile::arm32) code = thumb ? std::array<u8, 4>{0x00, 0xdf} : std::array<u8, 4>{0x00, 0x00, 0x00, 0xef};
    if constexpr (profile == lu::Profile::rv32 || profile == lu::Profile::rv64) code = {0x73, 0, 0, 0};
    if constexpr (profile == lu::Profile::loongarch64) code = {0, 0, 0x2b, 0};
    if constexpr (profile == lu::Profile::mips || profile == lu::Profile::mips64) code = {0, 0, 0, 0x0c};
    if constexpr (profile == lu::Profile::ppc) code = {0x44, 0, 0, 0x02};
    for (usize i = 0; i < instruction_bytes(thumb); ++i) mock::memory[site - hw::user_begin + i] = std::byte{code[i]};
}
hw::UserFrame read_frame(i32 fd, uptr site = hw::user_begin + 1024, usize count = 3, bool thumb = false)
{
    hw::UserFrame frame{};
    for (usize i = 0; i < frame.registers.size(); ++i) frame.registers[i] = 0x1100 + i;
    for (usize i = 0; i < frame.fp_state.size(); ++i) frame.fp_state[i] = static_cast<u8>(i * 7);
    frame.pc = site + instruction_bytes(thumb); frame.sp = hw::user_begin + 8192;
    frame.fs_base = 0x7654; frame.gs_base = 0x4567;
    if constexpr (profile == lu::Profile::x86_64)
    { frame.registers[14] = 0; frame.registers[9] = fd; frame.registers[10] = output; frame.registers[11] = count; }
    else if constexpr (profile == lu::Profile::i386)
    { frame.registers[7] = 3; frame.registers[4] = fd; frame.registers[6] = output; frame.registers[5] = count; }
    else if constexpr (profile == lu::Profile::aarch64 || profile == lu::Profile::arm32)
    {
        frame.registers[profile == lu::Profile::arm32 ? 7 : 8] = profile == lu::Profile::arm32 ? 3 : 63;
        frame.registers[0] = fd; frame.registers[1] = output; frame.registers[2] = count;
        if constexpr (profile == lu::Profile::arm32) { frame.flags = thumb ? 0x30 : 0x10; frame.registers[15] = frame.pc; }
    }
    else if constexpr (profile == lu::Profile::rv32 || profile == lu::Profile::rv64)
    { frame.registers[17] = 63; frame.registers[10] = fd; frame.registers[11] = output; frame.registers[12] = count; }
    else if constexpr (profile == lu::Profile::loongarch64)
    { frame.registers[11] = 63; frame.registers[4] = fd; frame.registers[5] = output; frame.registers[6] = count; }
    else if constexpr (profile == lu::Profile::mips || profile == lu::Profile::mips64)
    { frame.registers[2] = profile == lu::Profile::mips ? 4003 : 5000; frame.registers[4] = fd; frame.registers[5] = output; frame.registers[6] = count; frame.registers[7] = 0x12345678; }
    else { frame.registers[0] = 3; frame.registers[3] = fd; frame.registers[4] = output; frame.registers[5] = count; }
    opcode(site, thumb); return frame;
}
auto binding(Kernel &, Group g)
{
    auto object = native::pipe_read_object(g.pid, g.reader); CHECK(object.pool);
    auto found = native::pipe_read_objects.resolve(object); CHECK(found);
    return native::pipe_origin_binding(object, *found.value());
}
auto origin(Kernel &kernel, Group g)
{ return native::syscall_origins.identity(*kernel.scheduler().find(g.pid), *kernel.scheduler().find_thread(g.pid, g.reader)); }
void put(Kernel &kernel, i32 fd, char value)
{ const std::array<std::byte, 1> data{std::byte{static_cast<u8>(value)}}; auto written = kernel.posix().write(fd, data); CHECK(written && written.value() == 1); }
void step_arm(Kernel &kernel, Group g, i32 fd, bool thumb = false)
{
    auto *thread = kernel.scheduler().find_thread(g.pid, g.reader);
    thread->native_frame = read_frame(fd, hw::user_begin + 1024, 3, thumb);
    mock::entry_trap = syscall_trap();
    native::last_pid = native::last_tid = 0;
    const auto entries = mock::entries;
    expect(native::step(kernel)); expect(mock::entries == entries + 1);
    expect(thread->syscall_pending && thread->wait_reason == sched::ThreadWaitReason::pipe_read);
    expect(native::syscall_origins.live(origin(kernel, g)));
}
void lifecycle(Kernel &kernel, bool thumb)
{
    auto first = create(kernel), g = create(kernel);
    kernel.scheduler().find_thread(first.pid, first.reader)->native_active = false;
    auto a = kernel.posix().pipe(0), b = kernel.posix().pipe(0); CHECK(a && b);
    step_arm(kernel, g, a.value()[0], thumb);
    auto *p = kernel.scheduler().find(g.pid); auto *thread = kernel.scheduler().find_thread(g.pid, g.reader);
    const auto captured = thread->native_frame;
    auto id = origin(kernel, g); auto wait = binding(kernel, g);
    auto permit = native::syscall_origins.prepare_restart(id, captured, wait); expect(permit.ok());
    expect(Origins::same_frame(permit.value().capture(), captured));
    expect(permit.value().plan().frame.pc == captured.pc - instruction_bytes(thumb));
    expect(permit.value().identity().epoch && permit.value().identity().lease == thread->entry_stack);
    expect(!native::syscall_origins.validate_restart(Origins::RestartPermit{}, id, captured, wait));
    for (unsigned change = 0; change < 12; ++change)
    {
        auto wrong = id;
        switch (change)
        {
        case 0: ++wrong.pid; break; case 1: ++wrong.tid; break; case 2: ++wrong.epoch; break;
        case 3: ++wrong.image; break; case 4: ++wrong.capture; break; case 5: ++wrong.lease.generation; break;
        case 6: wrong.lease.slot = (wrong.lease.slot + 1) % Origins::capacity; break;
        case 7: wrong.lease.pool = nullptr; break; case 8: ++wrong.space.root; break;
        case 9: ++wrong.space.slot; break; case 10: ++wrong.space.generation; break; case 11: ++wrong.top; break;
        }
        expect(!native::syscall_origins.prepare_restart(wrong, captured, wait));
        expect(!native::syscall_origins.consume_restart(permit.value(), wrong, captured, wait));
    }
    for (unsigned change = 0; change < 9; ++change)
    {
        auto wrong = wait;
        switch (change)
        {
        case 0: ++wrong.ticket.generation; break; case 1: ++wrong.object.generation; break;
        case 2: ++wrong.description; break; case 3: ++wrong.pipe; break; case 4: ++wrong.output; break;
        case 5: ++wrong.count; break; case 6: ++wrong.fd; break; case 7: ++wrong.request; break;
        case 8: wrong.object.pool = nullptr; break;
        }
        expect(!native::syscall_origins.prepare_restart(id, captured, wrong));
    }
    for (unsigned field = 0; field < 7; ++field)
    {
        auto wrong = captured;
        if (field == 0) ++wrong.pc; else if (field == 1) ++wrong.sp; else if (field == 2) ++wrong.flags;
        else if (field == 3) ++wrong.fs_base; else if (field == 4) ++wrong.gs_base;
        else if (field == 5) ++wrong.registers[0]; else ++wrong.fp_state[511];
        expect(!native::syscall_origins.prepare_restart(id, wrong, wait));
        thread->native_frame = wrong;
        expect(!native::syscall_origins.prepare_restart(id, captured, wait));
        thread->native_frame = captured;
    }
    mock::memory[captured.pc - instruction_bytes(thumb) - hw::user_begin] = std::byte{0xff};
    expect(native::syscall_origins.validate_restart(permit.value(), id, captured, wait));
    // Numeric fd reuse wakes A spuriously. Rearm keeps capture and pinned A.
    leader(kernel, g); CHECK(kernel.posix().duplicate_to(b.value()[0], a.value()[0])); put(kernel, b.value()[1], 'B');
    const auto entries = mock::entries; expect(native::step(kernel)); expect(mock::entries == entries);
    const auto rearmed = binding(kernel, g);
    expect(origin(kernel, g) == id && rearmed.ticket.generation > wait.ticket.generation);
    expect(rearmed.description == wait.description && rearmed.object == wait.object && rearmed.request == wait.request);
    expect(!native::syscall_origins.validate_restart(permit.value(), id, captured, rearmed));
    expect(!native::syscall_origins.prepare_restart(id, captured, wait));
    wait = rearmed; permit = native::syscall_origins.prepare_restart(id, captured, wait); expect(permit.ok());
    // A returnable failed exec must preserve peer authority and the exact pin.
    leader(kernel, g); std::strcpy(reinterpret_cast<char *>(mock::memory.data() + 512), "/missing-origin-image");
    bool complete = true;
    expect(native::dispatch(kernel, g.pid, {OK_SYS_EXECVE, {hw::user_begin + 512, 0, 0, 0, 0, 0}}, complete) < 0 && complete);
    expect(native::syscall_origins.validate_restart(permit.value(), id, captured, wait));
    const auto failed = native::replace_entry_stacks(native::entry_stacks, *p, [&] { return Status::denied("exec control"); });
    expect(!failed && !failed.committed && native::syscall_origins.validate_restart(permit.value(), id, captured, wait));
    // Process compaction moves the canonical frame; no stored frame pointer is used.
    auto *old_address = thread;
    native::release_process(kernel, first.pid); CHECK(kernel.scheduler().exit_process(first.pid, 0)); CHECK(kernel.scheduler().reap_orphans());
    p = kernel.scheduler().find(g.pid); thread = kernel.scheduler().find_thread(g.pid, g.reader);
    expect(thread != old_address && origin(kernel, g) == id);
    expect(native::syscall_origins.validate_restart(permit.value(), id, thread->native_frame, wait));
    // Actual runtime table replacement: failed destroy does not alter origins.
    mock::spaces_enabled = true; const auto previous_space = p->resources().native_space;
    mock::fail_destroy_root = previous_space.root; bool can_release = true;
    auto staged = native::Pages::clone(p->resources().native_pages, native::page_pool); CHECK(staged);
    auto pages = std::move(staged.value());
    expect(!native::replace_native_pages(p->resources(), pages, can_release) && can_release);
    expect(Origins::same_space(p->resources().native_space, previous_space));
    expect(native::syscall_origins.validate_restart(permit.value(), id, captured, wait));
    expect(native::replace_native_pages(p->resources(), pages, can_release).ok());
    const auto new_id = origin(kernel, g);
    expect(!Origins::same_space(new_id.space, id.space) && new_id.capture == id.capture && new_id.image == id.image);
    expect(!native::syscall_origins.validate_restart(permit.value(), new_id, captured, wait));
    expect(!native::syscall_origins.prepare_restart(id, captured, wait));
    id = new_id; permit = native::syscall_origins.prepare_restart(id, captured, wait); expect(permit.ok());
    // A fresh child lease with copied registers does not inherit capture authority.
    auto child = kernel.scheduler().create_thread(g.pid, thread->context); CHECK(child);
    auto *created = kernel.scheduler().find_thread(g.pid, child.value()); CHECK(native::prepare_native_thread(*created));
    created->native_frame = captured; created->native_active = true;
    expect(!native::syscall_origins.live(native::syscall_origins.identity(*p, *created)));
    CHECK(native::retire_native_thread(*created)); CHECK(kernel.scheduler().exit_thread(g.pid, child.value()));
    // Final data completion consumes A's authority without entering userspace again.
    leader(kernel, g); put(kernel, a.value()[1], 'A'); expect(native::step(kernel));
    expect(mock::memory[128] == std::byte{'A'} && !native::syscall_origins.live(id));
    expect(!native::syscall_origins.validate_restart(permit.value(), id, captured, wait));
    expect(!native::pipe_read_object(g.pid, g.reader).pool);
    leader(kernel, g); std::array<std::byte, 1> left{}; auto read = kernel.posix().read(b.value()[0], left);
    expect(read && read.value() == 1 && left[0] == std::byte{'B'});
    retire(kernel, g); mock::spaces_enabled = false;
}

// Directly exercises the production wrapper around hosted enter_owned, with no
// API accepting a caller-supplied Trap. No target CPU executes in these tests.
void admission(Kernel &kernel, unsigned mode)
{
    auto g = create(kernel); auto *p = kernel.scheduler().find(g.pid); auto *thread = kernel.scheduler().find_thread(g.pid, g.reader);
    uptr site = hw::user_begin + 4092;
    constexpr bool crossing = profile == lu::Profile::i386 || profile == lu::Profile::x86_64 ||
        profile == lu::Profile::rv32 || profile == lu::Profile::rv64;
    if constexpr (crossing) site = hw::user_begin + (instruction_bytes() == 2 ? 4095 : 4094);
    thread->native_frame = read_frame(3, site); mock::entry_trap = syscall_trap();
    if (mode == 1) p->resources().native_pages[0].flags = hw::page_read;
    if (mode == 2) p->resources().native_pages[1].flags = hw::page_read;
    if (mode == 3) mock::forbidden = thread->native_frame.pc - 1;
    if (mode == 4)
        for (usize i = 0; i < instruction_bytes(); ++i) mock::memory[site - hw::user_begin + i] = std::byte{};
    if (mode == 5) mock::entry_trap.kind = hw::TrapKind::timer;
    if (mode == 6) mock::entry_trap.vector = 0xffff;
    if (mode == 7) mock::invalid_capture = true;
    if (mode == 8) mock::entry_trap.error_code |= 0x80000000;
    auto active = native::entry_stacks.activate_entry(thread->entry_stack, {g.pid, g.reader}, 0); CHECK(active);
    const auto copies = mock::copies;
    auto trap = native::syscall_origins.enter(*p, *thread, active.value(), kernel.scheduler());
    const auto id_while_active = origin(kernel, g);
    expect(!native::syscall_origins.live(id_while_active));
    CHECK(native::entry_stacks.deactivate_entry(thread->entry_stack, {g.pid, g.reader}, 0));
    constexpr bool mips = profile == lu::Profile::mips || profile == lu::Profile::mips64;
    const bool valid = mode == 0 || (mode == 2 && !crossing) || (mode == 8 && profile == lu::Profile::ppc);
    expect(native::syscall_origins.live(origin(kernel, g)) == valid);
    expect(mode == 7 ? !trap : trap.ok());
    if (mode == 0) expect(mock::copies == copies + 1 && mock::last_input == site && mock::last_input_size == instruction_bytes());
    if (mode == 8 && mips) expect(mock::copies == copies);
    mock::forbidden = 0; mock::invalid_capture = false; retire(kernel, g);
}
void stale_views(Kernel &kernel)
{
    auto g = create(kernel); auto *p = kernel.scheduler().find(g.pid); auto *thread = kernel.scheduler().find_thread(g.pid, g.reader);
    thread->native_frame = read_frame(3); mock::entry_trap = syscall_trap();
    alignas(4096) static std::array<std::byte, Origins::Pool::arena_bytes> other_arena;
    Origins::Pool other; CHECK(other.activate(other_arena));
    auto dummy = other.reserve({g.pid + 10000, g.reader + 10000}); CHECK(dummy);
    auto lease = other.reserve({g.pid, g.reader}); CHECK(lease);
    while (lease.value().generation < thread->entry_stack.generation)
    {
        CHECK(other.release(lease.value(), {g.pid, g.reader}));
        lease = other.reserve({g.pid, g.reader}); CHECK(lease);
    }
    auto view = native::entry_stacks.activate_entry(thread->entry_stack, {g.pid, g.reader}, 0); CHECK(view);
    auto alien = other.activate_entry(lease.value(), {g.pid, g.reader}, 0); CHECK(alien);
    while (other.accounting().activations < native::entry_stacks.accounting().activations)
    {
        CHECK(other.deactivate_entry(lease.value(), {g.pid, g.reader}, 0));
        alien = other.activate_entry(lease.value(), {g.pid, g.reader}, 0); CHECK(alien);
    }
    expect(lease.value().generation == thread->entry_stack.generation && lease.value().slot == thread->entry_stack.slot);
    expect(other.accounting().activations == native::entry_stacks.accounting().activations);
    const auto entries = mock::entries;
    expect(!native::syscall_origins.enter(*p, *thread, alien.value(), kernel.scheduler())); expect(mock::entries == entries);
    CHECK(other.deactivate_entry(lease.value(), {g.pid, g.reader}, 0)); CHECK(other.release(lease.value(), {g.pid, g.reader}));
    CHECK(other.release(dummy.value(), {g.pid + 10000, g.reader + 10000}));
    CHECK(native::entry_stacks.deactivate_entry(thread->entry_stack, {g.pid, g.reader}, 0));
    auto fresh = native::entry_stacks.activate_entry(thread->entry_stack, {g.pid, g.reader}, 0); CHECK(fresh);
    expect(!native::syscall_origins.enter(*p, *thread, view.value(), kernel.scheduler())); expect(mock::entries == entries);
    CHECK(native::syscall_origins.enter(*p, *thread, fresh.value(), kernel.scheduler()));
    CHECK(native::entry_stacks.deactivate_entry(thread->entry_stack, {g.pid, g.reader}, 0));
    auto id = origin(kernel, g); expect(native::syscall_origins.live(id));
    const auto old_capture = native::SyscallOriginTestAccess::capture(native::syscall_origins, ~u64{0});
    fresh = native::entry_stacks.activate_entry(thread->entry_stack, {g.pid, g.reader}, 0); CHECK(fresh);
    expect(!native::syscall_origins.enter(*p, *thread, fresh.value(), kernel.scheduler()));
    CHECK(native::entry_stacks.deactivate_entry(thread->entry_stack, {g.pid, g.reader}, 0));
    expect(!native::syscall_origins.live(id)); native::SyscallOriginTestAccess::capture(native::syscall_origins, old_capture);
    const auto old_image = native::SyscallOriginTestAccess::image(native::syscall_origins, ~u64{0});
    expect(!native::syscall_origins.prepare(*thread)); expect(!native::syscall_origins.live(id));
    native::SyscallOriginTestAccess::image(native::syscall_origins, old_image);
    CHECK(native::syscall_origins.prepare(*thread));
    // Re-entered activation invalidates the old view even when lease and owner
    // remain the same throughout the synthetic backend callback.
    static sched::ThreadControlBlock *rotating;
    rotating = thread;
    mock::owned_hook = [](hw::AddressSpace &, hw::UserFrame &, const hw::KernelEntryStackView &) {
        CHECK(native::entry_stacks.deactivate_entry(rotating->entry_stack, {rotating->owner, rotating->tid}, 0));
        CHECK(native::entry_stacks.activate_entry(rotating->entry_stack, {rotating->owner, rotating->tid}, 0));
    };
    fresh = native::entry_stacks.activate_entry(thread->entry_stack, {g.pid, g.reader}, 0); CHECK(fresh);
    expect(!native::syscall_origins.enter(*p, *thread, fresh.value(), kernel.scheduler()));
    CHECK(native::entry_stacks.deactivate_entry(thread->entry_stack, {g.pid, g.reader}, 0));
    mock::owned_hook = nullptr;
    expect(!native::syscall_origins.live(origin(kernel, g)));
    retire(kernel, g);
}
void consume_and_exec(Kernel &kernel, unsigned mode)
{
    auto g = create(kernel); auto a = kernel.posix().pipe(0); CHECK(a); step_arm(kernel, g, a.value()[0]);
    auto *p = kernel.scheduler().find(g.pid); auto *thread = kernel.scheduler().find_thread(g.pid, g.reader);
    const auto id = origin(kernel, g); const auto wait = binding(kernel, g); const auto frame = thread->native_frame;
    auto permit = native::syscall_origins.prepare_restart(id, frame, wait); expect(permit.ok());
    if (mode == 0)
    {
        expect(native::syscall_origins.consume_restart(permit.value(), id, frame, wait));
        expect(!native::syscall_origins.consume_restart(permit.value(), id, frame, wait));
    }
    else if (mode == 1)
    {
        native::ExecRetirement retirement{*p, g.pid};
        auto replaced = native::replace_entry_stacks(native::entry_stacks, *p,
            [&] { return kernel.posix().exec(g.pid, p->threads()[0].context, p->resources().memory); });
        expect(replaced && replaced.committed); retirement.consume(kernel, *p);
        expect(!native::pipe_read_object(g.pid, g.reader).pool);
        auto image_retirement = native::ImageRetirementReservation::reserve(); CHECK(image_retirement);
        native::Image image{std::move(image_retirement.value())};
        image.personality = sched::UserPersonality::linux;
        image.space = p->resources().native_space; ++image.space.generation;
        for (const auto &page : p->resources().native_pages)
        {
            CHECK(native::allocate(image.pages, page.virtual_address, page.flags));
            std::memcpy(reinterpret_cast<void *>(image.pages[image.pages.size() - 1].physical_address),
                reinterpret_cast<const void *>(page.physical_address), hw::page_size);
        }
        image.frame = read_frame(a.value()[0]);
        native::release(p->resources().native_space, p->resources().native_pages);
        CHECK(native::install(*p, image));
        expect(!native::syscall_origins.live(native::syscall_origins.identity(*p, p->threads()[0])));
        Group replacement{g.pid, g.pid}; step_arm(kernel, replacement, a.value()[0]);
        const auto replacement_id = origin(kernel, replacement);
        expect(replacement_id.image > id.image && replacement_id.capture > id.capture);
        expect(!native::syscall_origins.validate_restart(permit.value(), replacement_id,
            p->threads()[0].native_frame, binding(kernel, replacement)));
    }
    else if (mode == 2) { CHECK(native::retire_native_thread(*thread)); CHECK(kernel.scheduler().exit_thread(g.pid, g.reader)); native::prune_pipe_reads(kernel); }
    else if (mode == 3) { CHECK(native::set_result(*p, thread->native_frame, -4)); }
    else
    {
        CHECK(kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::cancelled));
        CHECK(kernel.scheduler().activate_thread(g.pid, g.reader));
        auto next = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::deadline, 99); CHECK(next);
        expect(!native::syscall_origins.prepare_restart(id, frame, wait)); // Before the cleanup sweep.
        native::prune_pipe_reads(kernel); expect(thread->wait_registration.pending(next.value()));
    }
    expect(!native::syscall_origins.prepare_restart(id, frame, wait));
    retire(kernel, g);
}
void shared_transaction(Kernel &kernel)
{
    auto g = create(kernel); auto a = kernel.posix().pipe(0), b = kernel.posix().pipe(0); CHECK(a && b);
    step_arm(kernel, g, a.value()[0]);
    auto *p = kernel.scheduler().find(g.pid);
    auto made = kernel.scheduler().create_thread(g.pid, p->threads()[0].context); CHECK(made);
    auto *second = kernel.scheduler().find_thread(g.pid, made.value()); CHECK(native::prepare_native_thread(*second)); second->native_active = true;
    Group h{g.pid, made.value()}; step_arm(kernel, h, b.value()[0]);
    auto *first = kernel.scheduler().find_thread(g.pid, g.reader); second = kernel.scheduler().find_thread(h.pid, h.reader);
    const auto first_id = origin(kernel, g), second_id = origin(kernel, h);
    const auto first_wait = binding(kernel, g), second_wait = binding(kernel, h);
    const auto first_frame = first->native_frame, second_frame = second->native_frame;
    const auto first_permit = native::syscall_origins.prepare_restart(first_id, first_frame, first_wait);
    const auto second_permit = native::syscall_origins.prepare_restart(second_id, second_frame, second_wait);
    expect(first_permit && second_permit);
    const auto previous = p->resources().native_space;
    const hw::AddressSpace next{previous.root + 0x200000, previous.slot, previous.generation + 1};
    auto transaction = native::syscall_origins.stage_space(&kernel.scheduler(), p->resources(), previous, next); expect(transaction.ok());
    // A late second-participant failure cannot update the first origin.
    p->resources().native_space = next; ++second->native_frame.flags;
    expect(!native::syscall_origins.commit_space(transaction.value(), &kernel.scheduler(), p->resources()));
    p->resources().native_space = previous; second->native_frame = second_frame;
    expect(native::syscall_origins.validate_restart(first_permit.value(), first_id, first_frame, first_wait));
    expect(native::syscall_origins.validate_restart(second_permit.value(), second_id, second_frame, second_wait));
    mock::spaces_enabled = true; bool can_release = true;
    auto staged = native::Pages::clone(p->resources().native_pages, native::page_pool); CHECK(staged);
    auto pages = std::move(staged.value());
    ++second->native_frame.flags;
    expect(!native::replace_native_pages(p->resources(), pages, can_release));
    expect(Origins::same_space(previous, p->resources().native_space));
    second->native_frame = second_frame;
    expect(native::syscall_origins.validate_restart(first_permit.value(), first_id, first_frame, first_wait));
    expect(native::syscall_origins.validate_restart(second_permit.value(), second_id, second_frame, second_wait));
    expect(native::replace_native_pages(p->resources(), pages, can_release).ok());
    const auto updated_first = origin(kernel, g), updated_second = origin(kernel, h);
    expect(Origins::same_space(updated_first.space, updated_second.space));
    expect(!Origins::same_space(updated_first.space, previous));
    expect(updated_first.capture == first_id.capture && updated_second.capture == second_id.capture);
    expect(!native::syscall_origins.prepare_restart(first_id, first_frame, first_wait));
    expect(!native::syscall_origins.prepare_restart(second_id, second_frame, second_wait));
    expect(native::syscall_origins.prepare_restart(updated_first, first_frame, first_wait).ok());
    expect(native::syscall_origins.prepare_restart(updated_second, second_frame, second_wait).ok());
    leader(kernel, g); put(kernel, a.value()[1], '1'); put(kernel, b.value()[1], '2');
    expect(native::step(kernel)); expect(native::step(kernel));
    expect(!native::syscall_origins.live(updated_first) && !native::syscall_origins.live(updated_second));
    retire(kernel, g); mock::spaces_enabled = false;
}
void retained_epoch(Kernel &kernel)
{
    auto g = create(kernel); auto pipe = kernel.posix().pipe(0); CHECK(pipe);
    step_arm(kernel, g, pipe.value()[0]);
    auto *process = kernel.scheduler().find(g.pid);
    auto *thread = kernel.scheduler().find_thread(g.pid, g.reader);
    const auto id = origin(kernel, g), captured = id;
    const auto wait = binding(kernel, g);
    const auto frame = thread->native_frame;
    auto permit = native::syscall_origins.prepare_restart(id, frame, wait); expect(permit.ok());
    const auto view = native::entry_stacks.activate_entry(thread->entry_stack, {g.pid, g.reader}, 0); CHECK(view);
    expect(!native::syscall_origins.validate_restart(permit.value(), id, frame, wait));
    CHECK(native::entry_stacks.deactivate_entry(thread->entry_stack, {g.pid, g.reader}, 0));
    expect(origin(kernel, g) == captured);
    expect(!native::syscall_origins.prepare_restart(id, frame, wait));
    expect(!native::syscall_origins.consume_restart(permit.value(), id, frame, wait));
    const auto previous = process->resources().native_space;
    const hw::AddressSpace next{previous.root + 0x200000, previous.slot, previous.generation + 1};
    expect(!native::syscall_origins.stage_space(&kernel.scheduler(), process->resources(), previous, next));
    retire(kernel, g);
}
void immediate(Kernel &kernel)
{
    auto g = create(kernel); auto a = kernel.posix().pipe(0); CHECK(a);
    auto *thread = kernel.scheduler().find_thread(g.pid, g.reader);
    for (unsigned mode = 0; mode < 3; ++mode)
    {
        leader(kernel, g);
        if (mode == 1) { CHECK(kernel.posix().fcntl(a.value()[0], posix::f_SETFL, posix::o_NONBLOCK)); }
        if (mode == 2) { CHECK(kernel.posix().fcntl(a.value()[0], posix::f_SETFL, 0)); put(kernel, a.value()[1], 'I'); }
        thread->native_frame = read_frame(a.value()[0], hw::user_begin + 1024, mode == 0 ? 0 : 3);
        mock::entry_trap = syscall_trap(); native::last_pid = native::last_tid = 0;
        const auto entries = mock::entries; expect(native::step(kernel)); expect(mock::entries == entries + 1);
        expect(!thread->syscall_pending && !native::pipe_read_object(g.pid, g.reader).pool);
        expect(!native::syscall_origins.live(origin(kernel, g)));
    }
    expect(mock::memory[128] == std::byte{'I'}); retire(kernel, g);
}
} // namespace
int main()
{
    static Kernel kernel; CHECK(kernel.boot({})); CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    lifecycle(kernel, false);
    if constexpr (profile == lu::Profile::arm32) lifecycle(kernel, true);
    for (unsigned mode = 0; mode < 9; ++mode) admission(kernel, mode);
    stale_views(kernel);
    for (unsigned mode = 0; mode < 5; ++mode) consume_and_exec(kernel, mode);
    shared_transaction(kernel); immediate(kernel); retained_epoch(kernel);
    expect(native::entry_stacks.accounting().leased_slots == 0);
    const auto objects = native::pipe_read_objects.accounting(); expect(objects.allocations == objects.destructions);
    std::printf("SYSCALL_ORIGIN_PASS hosted=1 native=0 checks=%zu captures=%zu balanced_objects=%llu\n",
        checks, mock::entries, static_cast<unsigned long long>(objects.allocations));
}
