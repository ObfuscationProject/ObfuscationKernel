// Actual clock/sleep adapters with hosted mappings; shared backend retains existing tests.
#include "native_clock_model.hpp"

namespace {
template <lu::Profile P>
void sleeps(Kernel &kernel, sched::ProcessId pid, usize number_slot, std::array<usize,4> slots)
{
    auto *process=kernel.scheduler().find(pid);auto &thread=process->threads()[0];
    constexpr uptr request=hw::user_begin+128, remainder=hw::user_begin+256;
    auto record=[&](i64 seconds,u64 nanoseconds,bool time64) {
        mock::memory.fill(std::byte{0x55});
        const usize width=time64?8:lu::Codec<P>::profile.word_bytes;
        const u64 values[2]={static_cast<u64>(seconds),nanoseconds};
        for(usize field=0;field<2;++field)for(usize i=0;i<width;++i)
            mock::memory[128+field*width+i]=static_cast<std::byte>(values[field]>>(8*(lu::Codec<P>::profile.big_endian?width-i-1:i)));
    };
    bool done=true;
    auto call=[&](lu::Operation op,std::array<u64,4> args) {
        auto nr=lu::Codec<P>::syscall_number(op);CHECK(nr);hw::UserFrame frame{};
        frame.registers[number_slot]=*nr;
        for(usize i=0;i<4;++i)frame.registers[slots[i]]=static_cast<uptr>(args[i]);
        done=true;
        return user::linux_bootstrap::dispatch<P>(kernel,pid,frame,done,
            [&](hw::Syscall c,bool &complete){return native::dispatch(kernel,pid,c,complete);});
    };
    auto retire=[&] {thread.native_deadline.cancel();CHECK(kernel.scheduler().set_runnable(pid));CHECK(kernel.posix().activate(pid));};
    using O=lu::Operation;
    for(O op:{O::nanosleep,O::nanosleep_time32,O::clock_nanosleep,O::clock_nanosleep_time32,O::clock_nanosleep_time64}) {
        if(!lu::Codec<P>::syscall_number(op))continue;
        const bool wide=op==O::clock_nanosleep_time64;
        const bool clocks=op!=O::nanosleep && op!=O::nanosleep_time32;
        auto invoke=[&](u64 clock=1,u64 flags=0,uptr ptr=hw::user_begin+128) {
            return clocks?call(op,{clock,flags,ptr,remainder}):call(op,{ptr,remainder,0,0});
        };
        record(0,17,wide);const auto before=mock::memory;const auto reads=mock::copies, outputs=mock::outputs;
        const auto generation=thread.native_deadline.generation();
        CHECK(invoke()==0 && !done && thread.native_deadline.armed());
        CHECK(thread.native_deadline.deadline()==mock::now+17 && thread.native_deadline.generation()==generation+1);
        CHECK(thread.wait_reason==sched::ThreadWaitReason::deadline && thread.wait_key==generation+1);
        CHECK(process->state()==sched::ProcessState::blocked && mock::copies==reads+1 && mock::outputs==outputs);
        CHECK(mock::memory==before);
        mock::memory.fill(std::byte{0xff}); // Stored deadline ignores later user mutation.
        CHECK(!thread.native_deadline.complete(mock::now+16,generation+1));
        CHECK(!thread.native_deadline.complete(mock::now+17,generation));
        CHECK(thread.native_deadline.complete(mock::now+17,generation+1));retire();
        record(0,0,wide);CHECK(invoke()==0 && done && !thread.native_deadline.armed());
        record(-1,0,wide);CHECK(invoke()==-22 && done && !thread.native_deadline.armed());
        record(0,1000000000,wide);CHECK(invoke()==-22 && done);
        record(0,~u64{0},wide);CHECK(invoke()==-22 && done);
        record(0,1,wide);mock::forbidden=request+3;CHECK(invoke()==-14 && done);mock::forbidden=0;
        CHECK(invoke(1,0,0)==-14 && done);
        mock::clock_fail=true;CHECK(invoke()==-5 && done);mock::clock_fail=false;
        if(clocks) {
            CHECK(invoke(99,0,0)==-22 && done); // Clock rejection precedes request copy.
            CHECK(invoke(0,1,0)==-22 && done); // Absolute realtime epoch not supplied.
            record(0,0,wide);CHECK(invoke(1,~u32{0})==0 && done); // Only ABSTIME is significant.
            record(0,17,wide);CHECK(invoke(0,0)==0 && !done);retire();
            record(0,17,wide);CHECK(invoke(1,2)==0 && !done && thread.native_deadline.deadline()==mock::now+17);retire();
            record(3,99,wide);CHECK(invoke(1,1)==0 && !done && thread.native_deadline.deadline()==3000000099);retire();
            if constexpr(lu::Codec<P>::profile.word_bytes==8) {
                record(0,0,wide);CHECK(invoke((u64{1}<<32)|1,(u64{1}<<32)|1)==0 && done);
            }
        }
        if(wide && lu::Codec<P>::profile.word_bytes==4) {
            record(0,0x1234567800000011ULL,true);
            CHECK(invoke()==0 && !done && thread.native_deadline.deadline()==mock::now+17);retire();
        }
        if(wide || lu::Codec<P>::profile.word_bytes==8) {
            record(__INT64_MAX__,999999999,wide);CHECK(invoke()==0 && !done);
            CHECK(thread.native_deadline.deadline()==lu::ktime_maximum);retire();
            record(9223372036LL,0,wide);CHECK(invoke()==0 && !done && thread.native_deadline.deadline()==lu::ktime_maximum);retire();
        }
    }
    for(O op:{O::clock_getres,O::clock_getres_time32,O::clock_getres_time64}) {
        if(!lu::Codec<P>::syscall_number(op))continue;
        const bool wide=op==O::clock_getres_time64;
        mock::memory.fill(std::byte{0x55});CHECK(call(op,{1,request+1,0,0})==0 && done);
        const usize width=wide?8:lu::Codec<P>::profile.word_bytes;
        for(usize i=0;i<2*width;++i) {
            const usize one=lu::Codec<P>::profile.big_endian?2*width-1:width;
            CHECK(mock::memory[129+i]==(i==one?std::byte{1}:std::byte{0}));
        }
        CHECK(mock::memory[128]==std::byte{0x55} && mock::memory[129+2*width]==std::byte{0x55});
        CHECK(call(op,{1,0,0,0})==0 && call(op,{99,0,0,0})==-22);
        mock::forbidden=request;CHECK(call(op,{1,request,0,0})==-14);mock::forbidden=0;
        if constexpr(lu::Codec<P>::profile.word_bytes==8) CHECK(call(op,{(u64{1}<<32)|1,0,0,0})==0);
    }
    usize clock_layouts = 0;
    for (O op : {O::clock_gettime, O::clock_gettime32, O::clock_gettime64,
                 O::clock_getres, O::clock_getres_time32, O::clock_getres_time64}) {
        if (!lu::Codec<P>::syscall_number(op)) continue;
        ++clock_layouts;
        const bool resolution = op == O::clock_getres || op == O::clock_getres_time32 || op == O::clock_getres_time64;
        const bool wide = op == O::clock_gettime64 || op == O::clock_getres_time64;
        const usize size = wide ? 16 : 2 * lu::Codec<P>::profile.word_bytes;
        const uptr exact = hw::user_begin + mock::memory.size() - size;
        for (StatusCode state : {StatusCode::not_initialized, StatusCode::io_error}) {
            mock::clock_fail = true; mock::clock_failure = state;
            mock::memory.fill(std::byte{0xa5});
            const auto before = mock::memory;
            const auto outputs = mock::outputs, inputs = mock::copies, reads = mock::clock_reads;
            CHECK(call(op, {1, exact, 0, 0}) == -5 && done);
            CHECK(mock::memory == before && mock::outputs == outputs && mock::copies == inputs);
            CHECK(mock::clock_reads == reads + 1);
            // Failure must not attempt a user copy even for an invalid address.
            CHECK(call(op, {1, 1, 0, 0}) == -5 && mock::outputs == outputs && mock::memory == before);
            const auto identity_reads = mock::clock_reads;
            CHECK(call(op, {0, exact, 0, 0}) == -5 && call(op, {99, 0, 0, 0}) == -22);
            CHECK(mock::clock_reads == identity_reads && mock::memory == before);
            if (resolution) {
                CHECK(call(op, {1, 0, 0, 0}) == 0 && done);
                CHECK(mock::clock_reads == identity_reads && mock::outputs == outputs && mock::memory == before);
            }
        }
        mock::clock_fail = false; mock::clock_failure = StatusCode::io_error;
        mock::memory.fill(std::byte{0xa5});
        const auto before = mock::memory;
        mock::forbidden = exact + size - 1;
        CHECK(call(op, {1, exact, 0, 0}) == -14 && done && mock::memory == before);
        mock::forbidden = 0;
        CHECK(call(op, {1, exact, 0, 0}) == 0 && done);
        CHECK(mock::memory[mock::memory.size() - size - 1] == std::byte{0xa5});
    }
    CHECK(clock_layouts >= 2);
    usize realtime_layouts = 0;
    for (O op : {O::clock_gettime, O::clock_gettime32, O::clock_gettime64,
                 O::clock_getres, O::clock_getres_time32, O::clock_getres_time64}) {
        if (!lu::Codec<P>::syscall_number(op)) continue;
        ++realtime_layouts;
        const bool resolution = op == O::clock_getres || op == O::clock_getres_time32 || op == O::clock_getres_time64;
        const bool wide = op == O::clock_gettime64 || op == O::clock_getres_time64;
        const usize width = wide ? 8 : lu::Codec<P>::profile.word_bytes, size = 2 * width;
        const uptr exact = hw::user_begin + mock::memory.size() - size;
        for (StatusCode state : {StatusCode::not_initialized, StatusCode::unsupported, StatusCode::io_error}) {
            mock::realtime_fail = true; mock::realtime_failure = state;
            mock::memory.fill(std::byte{0xa5}); const auto before = mock::memory;
            const auto outputs = mock::outputs;
            CHECK(call(op, {0, exact, 0, 0}) == -5 && done);
            CHECK(call(op, {0, 1, 0, 0}) == -5 && done);
            CHECK(mock::memory == before && mock::outputs == outputs);
            const auto reads = mock::realtime_reads;
            CHECK(call(op, {99, 0, 0, 0}) == -22 && mock::realtime_reads == reads);
            if (resolution) CHECK(call(op, {0, 0, 0, 0}) == 0 && mock::realtime_reads == reads);
        }
        mock::realtime_fail = false;
        for (i64 seconds : {-2147483649LL, -2147483648LL, -1LL, 0LL, 2147483647LL, 2147483648LL, 2214176523LL}) {
            mock::epoch = {seconds, 987654321};
            mock::memory.fill(std::byte{0xa5}); const auto before = mock::memory;
            const auto outputs = mock::outputs;
            const bool overflow = !resolution && width == 4 && (seconds < -2147483648LL || seconds > 2147483647LL);
            if (overflow) {
                CHECK(call(op, {0, exact, 0, 0}) == -75 && done);
                CHECK(call(op, {0, 1, 0, 0}) == -75 && done);
                CHECK(mock::memory == before && mock::outputs == outputs);
                continue;
            }
            mock::forbidden = exact + size - 1;
            CHECK(call(op, {0, exact, 0, 0}) == -14 && done && mock::memory == before);
            mock::forbidden = 0;
            CHECK(call(op, {0, exact, 0, 0}) == 0 && done);
            const u64 values[2]{resolution ? 0 : static_cast<u64>(seconds), resolution ? u64{1} : u64{987654321}};
            for (usize field = 0; field < 2; ++field) for (usize byte = 0; byte < width; ++byte) {
                const usize shift = 8 * (lu::Codec<P>::profile.big_endian ? width - byte - 1 : byte);
                CHECK(mock::memory[mock::memory.size() - size + field * width + byte] == static_cast<std::byte>(values[field] >> shift));
            }
            CHECK(mock::memory[mock::memory.size() - size - 1] == std::byte{0xa5});
        }
        mock::realtime_fail = true;
    }
    CHECK(realtime_layouts == clock_layouts);
    std::printf("PASS realtime adapter %s: layouts=%zu signed epochs, time32 overflow, unavailable/errors, NULL identity and exact copy\n",
                lu::Codec<P>::profile.name, realtime_layouts);
    // Native UAPI time64 has the same failure and NULL-getres policy.
    for (syscall::Number op : {syscall::Number::clock_gettime, syscall::Number::clock_getres}) {
        for (StatusCode state : {StatusCode::not_initialized, StatusCode::io_error}) {
            mock::clock_fail = true; mock::clock_failure = state;
            mock::memory.fill(std::byte{0xa5}); const auto before = mock::memory;
            const auto outputs = mock::outputs;
            hw::Syscall request{static_cast<uptr>(op), {1, hw::user_begin + mock::memory.size() - sizeof(posix::ClockTime)}};
            done = true; CHECK(native::dispatch(kernel, pid, request, done) == -5 && done);
            CHECK(mock::outputs == outputs && mock::memory == before);
            if (op == syscall::Number::clock_getres) {
                const auto reads = mock::clock_reads; request.arguments[1] = 0;
                CHECK(native::dispatch(kernel, pid, request, done) == 0);
                CHECK(mock::clock_reads == reads && mock::outputs == outputs && mock::memory == before);
            }
        }
    }
    mock::clock_fail = false; mock::clock_failure = StatusCode::io_error;
    std::printf("PASS clock availability %s: layouts=%zu uninitialized/invalidated no-output, NULL identity-only, exact-end/final-byte\n",
                lu::Codec<P>::profile.name, clock_layouts);
    std::printf("PASS Linux sleep %s: copied endian/time64 requests, scalar widths, deadlines, saturation, resolution and errors\n",lu::Codec<P>::profile.name);
}
void native_realtime(Kernel &kernel, sched::ProcessId pid)
{
    using N = syscall::Number;
    constexpr uptr exact = hw::user_begin + mock::memory.size() - sizeof(ok_timespec);
    bool done = true;
    auto call = [&](N op, uptr first, uptr second = 0) {
        return native::dispatch(kernel, pid, {static_cast<uptr>(op), {first, second}}, done);
    };
    for (StatusCode state : {StatusCode::not_initialized, StatusCode::unsupported, StatusCode::io_error}) {
        mock::realtime_fail = true; mock::realtime_failure = state;
        mock::memory.fill(std::byte{0xa5}); const auto before = mock::memory;
        const auto outputs = mock::outputs;
        CHECK(call(N::clock_gettime, 0, exact) == -5);
        CHECK(call(N::clock_getres, 0, exact) == -5);
        CHECK(call(N::time, exact) == -5 && call(N::time, 0) == -5);
        CHECK(call(N::gettimeofday, exact) == -5);
        CHECK(call(N::clock_gettime, 0, 1) == -5 && call(N::clock_getres, 0, 1) == -5);
        CHECK(call(N::time, 1) == -5 && call(N::gettimeofday, 1) == -5);
        CHECK(mock::outputs == outputs && mock::memory == before);
        const auto reads = mock::realtime_reads;
        CHECK(call(N::clock_getres, 0, 0) == 0 && call(N::gettimeofday, 0) == 0);
        CHECK(call(N::clock_getres, 99, 0) == -22 && mock::realtime_reads == reads);
    }
    mock::realtime_fail = false; mock::epoch = {2214176523, 987654321};
    mock::memory.fill(std::byte{0xa5}); const auto before = mock::memory;
    mock::forbidden = exact + sizeof(ok_timespec) - 1;
    CHECK(call(N::clock_gettime, 0, exact) == -14 && mock::memory == before);
    CHECK(call(N::gettimeofday, exact) == -14 && mock::memory == before);
    mock::forbidden = 0;
    CHECK(call(N::clock_gettime, 0, 0) == -14);
    CHECK(call(N::clock_gettime, 0, exact) == 0);
    ok_timespec ts{}; std::memcpy(&ts, mock::memory.data() + mock::memory.size() - sizeof(ts), sizeof(ts));
    CHECK(ts.seconds == mock::epoch.seconds && ts.nanoseconds == mock::epoch.nanoseconds);
    CHECK(call(N::gettimeofday, exact) == 0);
    ok_timeval tv{}; std::memcpy(&tv, mock::memory.data() + mock::memory.size() - sizeof(tv), sizeof(tv));
    CHECK(tv.seconds == mock::epoch.seconds && tv.microseconds == 987654);
    CHECK(call(N::time, exact) == mock::epoch.seconds);
    i64 seconds{}; std::memcpy(&seconds, mock::memory.data() + mock::memory.size() - sizeof(ts), sizeof(seconds));
    CHECK(seconds == mock::epoch.seconds);
    for (i64 sec : {-4095LL, -1LL}) {
        mock::epoch.seconds = sec;
        mock::memory.fill(std::byte{0xa5}); const auto negative_before = mock::memory; const auto outputs = mock::outputs;
        CHECK(call(N::time, exact) == -75 && call(N::time, 1) == -75);
        CHECK(mock::memory == negative_before && mock::outputs == outputs);
    }
    CHECK(call(N::clock_gettime, 0, exact) == 0 && call(N::gettimeofday, exact) == 0);
    mock::epoch.seconds = -4096;
    CHECK(call(N::time, exact) == -4096);
    std::memcpy(&seconds, mock::memory.data() + mock::memory.size() - sizeof(ts), sizeof(seconds));
    CHECK(seconds == -4096);
    for (i64 sec : {-2147483649LL, 2147483648LL, -4095LL, -1LL})
        CHECK(!time::native_time_seconds<4>({sec, 0}));
    for (i64 sec : {-2147483648LL, -4096LL, 0LL, 2147483647LL})
        CHECK(time::native_time_seconds<4>({sec, 0}).value() == sec);
    CHECK(time::native_time_seconds<8>({2214176523, 0}).value() == 2214176523);

    // Trusted-table UTC uses the same epoch; its monotonic/sleep model stays separate.
    mock::epoch = {1709210096, 987654321};
    posix::ClockTime trusted{};
    auto response = kernel.syscalls().dispatch({N::clock_gettime, pid, {0, reinterpret_cast<uptr>(&trusted)}});
    CHECK(response.status && trusted.seconds == mock::epoch.seconds && trusted.nanoseconds == mock::epoch.nanoseconds);
    trusted = {123, 456}; mock::realtime_fail = true;
    response = kernel.syscalls().dispatch({N::clock_gettime, pid, {0, reinterpret_cast<uptr>(&trusted)}});
    CHECK(!response.status && trusted.seconds == 123 && trusted.nanoseconds == 456);
    response = kernel.syscalls().dispatch({N::clock_gettime, pid, {1, reinterpret_cast<uptr>(&trusted)}});
    CHECK(response.status && trusted.seconds == kernel.posix().clock_gettime().seconds);
    std::puts("PASS native/trusted realtime: epoch records, legacy migration, exact copy, failure nonpublication and return-width limits");
}
// Public clock boundary regressions with hosted mappings and a typed clock provider.
void public_clock_boundaries(Kernel &kernel, sched::ProcessId pid)
{
    using N = syscall::Number;
    bool done = true;
    constexpr uptr pointer = hw::user_begin + 4088; // Complete record crosses a page.
    constexpr uptr last8 = hw::user_begin + mock::memory.size() - 8;
    auto call = [&](N op, uptr first, uptr second = 0) {
        return native::dispatch(kernel, pid, {static_cast<uptr>(op), {first, second}}, done);
    };
    auto trusted = [&](N op, u64 first, u64 second = 0) {
        return kernel.syscalls().dispatch({op, pid, {first, second}});
    };
    mock::clock_fail = false;
    mock::realtime_fail = false;
    mock::forbidden = 0;
    mock::epoch = {1709210096, 999999999};
    mock::memory.fill(std::byte{0x5a});
    CHECK(call(N::clock_gettime, 0, pointer) == 0);
    ok_timespec ts{};
    std::memcpy(&ts, mock::memory.data() + 4088, sizeof(ts));
    CHECK(ts.seconds == mock::epoch.seconds && ts.nanoseconds == 999999999);
    const auto crossed = mock::memory;
    mock::forbidden = pointer + 15;
    CHECK(call(N::clock_gettime, 0, pointer) == -14 && mock::memory == crossed);
    CHECK(call(N::gettimeofday, pointer) == -14 && mock::memory == crossed);
    mock::forbidden = last8 + 7;
    CHECK(call(N::time, last8) == -14 && mock::memory == crossed);
    mock::forbidden = 0;
    CHECK(call(N::time, last8) == mock::epoch.seconds);
    CHECK(call(N::time, last8 + 1) == -14);
    const auto reads = mock::realtime_reads;
    CHECK(call(N::clock_gettime, uptr{1} << 32, pointer) == -22);
    CHECK(call(N::clock_getres, uptr{1} << 32, 0) == -22);
    CHECK(mock::realtime_reads == reads);
    for (u32 ns : {0u, 1u, 999u, 1000u, 999999999u}) {
        mock::epoch.nanoseconds = ns;
        CHECK(call(N::gettimeofday, pointer, ~uptr{0}) == 0);
        ok_timeval tv{};
        std::memcpy(&tv, mock::memory.data() + 4088, sizeof(tv));
        CHECK(tv.seconds == mock::epoch.seconds && tv.microseconds == ns / 1000);
    }
    // A live epoch with an unavailable interpolation resolution must not publish zero.
    mock::clock_fail = true;
    const auto before_resolution = mock::memory;
    const auto outputs = mock::outputs;
    CHECK(call(N::clock_getres, 0, pointer) == -5 && mock::memory == before_resolution);
    CHECK(mock::outputs == outputs);
    const auto null_reads = mock::realtime_reads;
    CHECK(call(N::clock_getres, 0, 0) == 0 && mock::realtime_reads == null_reads);
    mock::clock_fail = false;

    // Typed signed64 endpoints remain records; only the native errno interval is refused.
    constexpr i64 maximum = 9223372036854775807LL;
    constexpr i64 minimum = -maximum - 1;
    for (i64 seconds : std::array<i64, 4>{minimum, -4096, 0, maximum}) {
        mock::epoch = {seconds, 999999999};
        CHECK(call(N::clock_gettime, 0, pointer) == 0);
        CHECK(call(N::gettimeofday, pointer) == 0);
        CHECK(call(N::time, last8) == seconds);
        i64 result{};
        std::memcpy(&result, mock::memory.data() + mock::memory.size() - 8, 8);
        CHECK(result == seconds);
    }
    posix::ClockTime model_clock{};
    ok_timeval model_timeval{};
    i64 model_time = 123;
    mock::epoch = {2214176523LL, 999999999};
    auto result = trusted(N::time, reinterpret_cast<uptr>(&model_time));
    CHECK(result.status && result.value == mock::epoch.seconds && model_time == mock::epoch.seconds);
    result = trusted(N::gettimeofday, reinterpret_cast<uptr>(&model_timeval), ~u64{0});
    CHECK(result.status && model_timeval.seconds == mock::epoch.seconds && model_timeval.microseconds == 999999);
    result = trusted(N::clock_getres, 0, reinterpret_cast<uptr>(&model_clock));
    CHECK(result.status && model_clock.seconds == 0 && model_clock.nanoseconds == 1);
    for (i64 seconds : {-4095LL, -1LL}) {
        mock::epoch.seconds = seconds;
        model_time = 123;
        result = trusted(N::time, reinterpret_cast<uptr>(&model_time));
        CHECK(result.status.code() == StatusCode::overflow && model_time == 123);
    }
    // Epoch arithmetic/continuity failure is EIO, distinct from time32 encoding overflow.
    mock::realtime_fail = true;
    mock::realtime_failure = StatusCode::overflow;
    model_time = 123; model_timeval = {123, 456}; model_clock = {123, 456};
    const auto failed_memory = mock::memory;
    const auto failed_outputs = mock::outputs;
    CHECK(call(N::clock_gettime, 0, 1) == -5 && call(N::clock_getres, 0, 1) == -5);
    CHECK(call(N::time, 1) == -5 && call(N::gettimeofday, 1) == -5);
    CHECK(mock::memory == failed_memory && mock::outputs == failed_outputs);
    result = trusted(N::time, reinterpret_cast<uptr>(&model_time));
    CHECK(result.status.code() == StatusCode::io_error && model_time == 123);
    result = trusted(N::gettimeofday, reinterpret_cast<uptr>(&model_timeval));
    CHECK(result.status.code() == StatusCode::io_error && model_timeval.seconds == 123 && model_timeval.microseconds == 456);
    result = trusted(N::clock_getres, 0, reinterpret_cast<uptr>(&model_clock));
    CHECK(result.status.code() == StatusCode::io_error && model_clock.seconds == 123 && model_clock.nanoseconds == 456);
    const auto identity_reads = mock::realtime_reads;
    CHECK(trusted(N::clock_getres, 0, 0).status && trusted(N::gettimeofday, 0).status);
    CHECK(mock::realtime_reads == identity_reads);
    mock::realtime_fail = true;
    mock::realtime_failure = StatusCode::not_initialized;
    std::puts("PASS hosted public clock boundaries: native page/last-byte atomic copies, signed64 endpoints, timeval truncation, zero-resolution refusal, trusted legacy paths, provider-overflow EIO ordering");
}

void runtime_timeout(Kernel &kernel,sched::ProcessId pid) {
    auto *process=kernel.scheduler().find(pid);auto &thread=process->threads()[0];
    CHECK(native::prepare_native_thread(thread));
    process->resources().personality=sched::UserPersonality::linux;
    thread.native_active=true;thread.syscall_pending=true;
    thread.native_frame={};thread.native_frame.registers[14]=230; // x86_64 clock_nanosleep
    thread.native_frame.registers[9]=1;thread.native_frame.registers[10]=0;
    thread.native_frame.registers[11]=hw::user_begin;thread.native_frame.registers[5]=~uptr{0};
    const posix::ClockTime request{0,17};std::memcpy(mock::memory.data(),&request,sizeof(request));
    bool done=true;
    CHECK(user::linux_bootstrap::dispatch<lu::Profile::x86_64>(kernel,pid,thread.native_frame,done,
        [&](hw::Syscall c,bool &complete){return native::dispatch(kernel,pid,c,complete);})==0 && !done);
    const auto copies=mock::copies;mock::memory.fill(std::byte{0xff});
    native::started=true;native::runtime_kernel=&kernel;
    mock::now+=16;CHECK(native::step(kernel) && mock::entries==0 && thread.syscall_pending);
    mock::now+=1;CHECK(native::step(kernel) && mock::entries==1 && !thread.syscall_pending);
    CHECK(mock::copies==copies && !thread.native_deadline.armed());
    CHECK(lu::NativeBridge<lu::Profile::x86_64>::decode_return(mock::last_frame).value().value==0);
    CHECK(kernel.posix().activate(pid));
    CHECK(native::begin_sleep(kernel,pid,mock::now,{1,0},false,done));
    const auto generation=thread.native_deadline.generation();native::release_process(kernel,pid);
    CHECK(!thread.native_deadline.armed() && !thread.native_active);
    CHECK(native::entry_stacks.accounting().leased_slots==0 && native::runtime_status());
    CHECK(!thread.native_deadline.complete(~u64{0},generation));
    native::started=false;native::runtime_kernel=nullptr;
    std::puts("PASS hosted runtime timeout: one completion, no redispatch/copy, process personality and teardown");
}
}
int main() {
    static Kernel kernel;CHECK(kernel.boot({}));
    CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    const auto context=kernel.arch().make_user_context({hw::user_begin,hw::user_end-4096});
    const auto made=kernel.scheduler().create_user_process("sleep-adapter",context);
    CHECK(made && kernel.posix().activate(made.value()));const auto pid=made.value();
    sleeps<lu::Profile::i386>(kernel,pid,7,{4,6,5,1});
    sleeps<lu::Profile::x86_64>(kernel,pid,14,{9,10,11,5});
    sleeps<lu::Profile::aarch64>(kernel,pid,8,{0,1,2,3});
    sleeps<lu::Profile::arm32>(kernel,pid,7,{0,1,2,3});
    sleeps<lu::Profile::rv64>(kernel,pid,17,{10,11,12,13});
    sleeps<lu::Profile::rv32>(kernel,pid,17,{10,11,12,13});
    sleeps<lu::Profile::loongarch64>(kernel,pid,11,{4,5,6,7});
    sleeps<lu::Profile::mips>(kernel,pid,2,{4,5,6,7});
    sleeps<lu::Profile::mips64>(kernel,pid,2,{4,5,6,7});
    sleeps<lu::Profile::ppc>(kernel,pid,0,{3,4,5,6});
    native_realtime(kernel,pid);
    public_clock_boundaries(kernel,pid);
    runtime_timeout(kernel,pid);
}
