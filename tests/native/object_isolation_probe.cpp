#include "ok/boot/qemu_runtime_frames.hpp"
#include "ok/arch/execution.hpp"
#include "ok/boot/raw_handoff.hpp"
#include "ok/user/native_page_pool.hpp"
#include "ok/memory/object_pool.hpp"
using word=__UINTPTR_TYPE__;
static void put(char c) {
#if defined(OK_ARCH_TARGET_X86_64) || defined(OK_ARCH_TARGET_I386)
    unsigned char ready;
    do {asm volatile("inb %1,%0":"=a"(ready):"Nd"(0x3fdu));}while(!(ready&32));
    asm volatile("outb %0,%1"::"a"(c),"Nd"(0x3f8u));
#elif defined(OK_ARCH_TARGET_ARM32) || defined(OK_ARCH_TARGET_AARCH64)
    auto *uart=reinterpret_cast<volatile unsigned*>(0x09000000);
    while(uart[6]&32) {}
    uart[0]=static_cast<unsigned char>(c);
#else
#if defined(OK_ARCH_TARGET_MIPS64)
    constexpr word base=0xffffffffb80003f8ull;
#elif defined(OK_ARCH_TARGET_MIPS)
    constexpr word base=0xb80003f8;
#elif defined(OK_ARCH_TARGET_PPC)
    constexpr word base=0xe0004500;
#elif defined(OK_ARCH_TARGET_LOONGARCH64)
    constexpr word base=0x1fe001e0;
#else
    constexpr word base=0x10000000;
#endif
    auto *uart=reinterpret_cast<volatile unsigned char*>(base);
    while(!(uart[5]&32)) {}
    uart[0]=static_cast<unsigned char>(c);
#endif
}
static void text(const char*s){while(*s)put(*s++);}
static void hex64(ok::u64 value){text("0x");for(int shift=60;shift>=0;shift-=4)put("0123456789abcdef"[(value>>shift)&15]);}
extern "C" void *memcpy(void *destination,const void *source,ok::usize n){auto *d=static_cast<unsigned char *>(destination);const auto *s=static_cast<const unsigned char *>(source);for(ok::usize i=0;i<n;++i)d[i]=s[i];return destination;}
extern "C" void *memset(void *destination,int value,ok::usize n){auto *d=static_cast<unsigned char *>(destination);for(ok::usize i=0;i<n;++i)d[i]=static_cast<unsigned char>(value);return destination;}
extern "C" int memcmp(const void *first,const void *second,ok::usize n){const auto *a=static_cast<const unsigned char *>(first),*b=static_cast<const unsigned char *>(second);for(ok::usize i=0;i<n;++i)if(a[i]!=b[i])return a[i]<b[i]?-1:1;return 0;}
extern "C" ok::usize strlen(const char *s){ok::usize n=0;while(s[n])++n;return n;}
extern "C" void *memchr(const void *source,int value,ok::usize n){const auto *s=static_cast<const unsigned char *>(source);for(ok::usize i=0;i<n;++i)if(s[i]==static_cast<unsigned char>(value))return const_cast<unsigned char *>(s+i);return nullptr;}
struct ProbeReader
{
    ok::usize calls{};
    ok::Result<std::span<const std::byte>> read_physical(ok::u64 address,ok::usize count)
    {
#if defined(OK_ARCH_TARGET_ARM32)
        if (!ok::boot::valid_arm32_entry(ok::boot::arm32_entry_state()))
            return ok::Status::fault("unqualified ARM entry state");
#endif
        ++calls;
        text("FIRMWARE_READ address=");hex64(address);text(" size=");hex64(count);put('\n');
        constexpr ok::u64 start=OK_PROBE_WINDOW_START, end=OK_PROBE_WINDOW_END;
        if(address<start||address>=end||ok::u64(count)>end-address||address>~ok::uptr{0})
            return ok::Status::fault("outside explicit QEMU harness mapped window");
#ifdef OK_PROBE_HEADER_ONLY
        if(count>40)return ok::Status::fault("injected shortened firmware mapping");
#endif
        return std::span<const std::byte>{reinterpret_cast<const std::byte *>(static_cast<ok::uptr>(address)),count};
    }
};

namespace hw = ok::arch::execution;
using namespace ok;
extern "C" char _kernel_start[], _kernel_end[];
extern "C" const u8 ok_object_user_begin[], ok_object_user_end[], ok_object_user_load[];
extern "C" void ok_platform_console_write_char(char c) { put(c); }
static memory::FrameAllocator frames;
static memory::FrameMetadataStorage<1024, 64> frame_metadata;
static user::native::NativePagePool pages;
struct Secret { u32 value = 0xc3917a25; std::array<u8, 60> padding{}; };
static constinit memory::ObjectPool<Secret, user::native::NativePagePool, 1> objects{pages};
#if defined(OK_ARCH_TARGET_MIPS) || defined(OK_ARCH_TARGET_MIPS64) || defined(OK_ARCH_TARGET_LOONGARCH64)
alignas(4096) static std::array<u8, 16 * 4096> fallback;
#endif
static void field(const char *name, u64 value) { text(" ");text(name);text("=");hex64(value); }
static Status activate_pages()
{
    using namespace boot;
    user::native::NativePagePool::Source source;
    Status status;
#if defined(OK_ARCH_TARGET_MIPS) || defined(OK_ARCH_TARGET_MIPS64) || defined(OK_ARCH_TARGET_LOONGARCH64)
    // These entry protocols do not yet supply qualified firmware RAM. Keep
    // linker backing and its supervisor alias explicit, including MIPS KSEG.
    status = hw::initialize();
    const memory::MemoryRegion region{reinterpret_cast<uptr>(fallback.data()), fallback.size(), memory::RegionType::usable};
    if (status) status = frames.initialize({&region, 1}, 4096, frame_metadata.bytes);
    source = user::native::NativePagePool::Source::linker_reserved;
    text("OBJECT_BACKING source=linker");field("base", region.base);field("size", region.size);put('\n');
#else
    static FirmwareMemory scratch, firmware;
    static QualifiedBootMemory workspace, qualified;
    ProbeReader reader;
    auto discovered = discover_firmware_memory<arch::Architecture::OK_PROBE_ARCH>(raw_boot_handoff(), BootProtocol::OK_PROBE_PROTOCOL, reader, scratch, firmware);
    status = discovered.status;
    QemuEntryMemoryPlan plan;
    const auto begin = reinterpret_cast<uptr>(_kernel_start), end = reinterpret_cast<uptr>(_kernel_end);
    if (status) status = qemu_runtime_memory_plan<arch::Architecture::OK_PROBE_ARCH>(firmware, {begin, end-begin}, {}, 1024, plan);
    if (status) status = qualify_boot_memory(firmware, {plan.reservations.begin(),plan.reservations.size()}, plan.policy, workspace, qualified);
    if (status) status = hw::initialize();
    if (status) status = initialize_qualified_frames(qualified, frames, frame_metadata.bytes);
    source = user::native::NativePagePool::Source::qualified_firmware;
    text("OBJECT_BACKING source=firmware");field("pages", qualified.page_count());field("status", static_cast<u32>(status.code()));put('\n');
    for (const auto range : qualified.managed()) {text("OBJECT_RANGE");field("base",range.address);field("size",range.size);put('\n');}
#endif
    return status ? pages.activate(frames, source, 1) : status;
}
static Status run_case(hw::AddressSpace &space, uptr data, uptr pointer, bool protected_load, const char *name)
{
    constexpr u32 sentinel = 0xa5b6c7d8;
    const u32 marker = 0, sample = 0x12345678;
    auto status = hw::copy_to_user(space, data, &pointer, sizeof(pointer));
    if (status) status = hw::copy_to_user(space, data+16, &sentinel, sizeof(sentinel));
    if (status) status = hw::copy_to_user(space, data+20, &marker, sizeof(marker));
    if (status) status = hw::copy_to_user(space, data+64, &sample, sizeof(sample));
    if (!status) return status;
    auto frame = hw::initial_frame(hw::user_begin, hw::user_end-16);
    hw::Trap trap{};
    bool stopped = false;
    for (usize attempt=0; attempt<128; ++attempt)
    {
        auto entered = hw::enter(space, frame);
        if (!entered) return entered.status();
        trap = entered.value();
        if (trap.kind != hw::TrapKind::timer) { stopped=true;break; }
    }
    u32 observed=0, completed=0;
    if (status) status = hw::copy_from_user(space, &observed, data+16, sizeof(observed));
    if (status) status = hw::copy_from_user(space, &completed, data+20, sizeof(completed));
    const auto fault_pc = hw::user_begin + static_cast<usize>(ok_object_user_load-ok_object_user_begin);
    if (status && (!stopped || (protected_load ?
        trap.kind != hw::TrapKind::page_fault || trap.fault_address != pointer || frame.pc != fault_pc || observed != sentinel || completed != 0 :
        trap.kind != hw::TrapKind::syscall || hw::syscall(frame).number != 65 || observed != sample || completed != 1)))
        status = Status::io_error("actual user load/trap control failed");
    text("OBJECT_CASE name=");text(name);field("pointer",pointer);field("kind",static_cast<u8>(trap.kind));field("vector",trap.vector);
    field("error",trap.error_code);field("fault",trap.fault_address);field("pc",frame.pc);field("load_pc",fault_pc);
    field("observed",observed);field("completed",completed);field("status",static_cast<u32>(status.code()));put('\n');
    return status;
}
static Status probe()
{
    if (auto status=activate_pages(); !status) return status;
    auto handle=objects.create();
    if (!handle) return handle.status();
    auto object=objects.resolve(handle.value());
    if (!object) return object.status();
    auto *secret=object.value();
    const uptr address=reinterpret_cast<uptr>(&secret->value);
    if (pages.owns(address & ~uptr{4095}) || pages.handle(address & ~uptr{4095}) || pages.object_count()!=1)
        return Status::fault("object purpose registry exposed a user page");
    std::array<uptr,3> backing{};
    Status status;
    usize count=0;
    for (;count<backing.size();++count)
    {
        auto page=pages.allocate();
        if (!page) {status=page.status();break;}
        backing[count]=page.value();memset(reinterpret_cast<void *>(page.value()),0,4096);
    }
    bool retired=true;
    if (status)
    {
        const auto code_size=static_cast<usize>(ok_object_user_end-ok_object_user_begin);
        if (!code_size || code_size>4096) status=Status::invalid_argument("user program size");
        else
        {
            memcpy(reinterpret_cast<void *>(backing[0]),ok_object_user_begin,code_size);
            const std::array<hw::PageMapping,3> maps{{
                {hw::user_begin,backing[0],hw::page_read|hw::page_execute},
                {hw::user_begin+4096,backing[1],hw::page_read|hw::page_write},
                {hw::user_end-4096,backing[2],hw::page_read|hw::page_write}}};
            auto space=hw::create_space(maps);
            if (!space) status=space.status();
            else
            {
                text("OBJECT_LIVE");field("address",address);field("secret",secret->value);field("objects",pages.object_count());field("users",pages.user_count());put('\n');
                if (hw::user_range(space.value(),address,sizeof(u32))) status=Status::fault("supervisor pointer accepted by copy policy");
                if (status) status=run_case(space.value(),hw::user_begin+4096,hw::user_begin+4096+64,false,"before");
                if (status) status=run_case(space.value(),hw::user_begin+4096,address,true,"protected");
                if (status) status=run_case(space.value(),hw::user_begin+4096,hw::user_begin+4096+64,false,"after");
                auto destroyed=hw::destroy_space(space.value());
                retired=destroyed.ok();if (!destroyed) status=destroyed;
            }
        }
    }
    if (secret->value!=0xc3917a25) status=Status::io_error("supervisor secret corrupted");
    // If table retirement fails, every page and object remains quarantined.
    if (retired)
    {
        for (usize i=0;i<count;++i) if (auto released=pages.release(backing[i]);!released) status=released;
        if (auto destroyed=objects.destroy(handle.value());!destroyed) status=destroyed;
        if (auto trimmed=objects.trim_empty_pages();!trimmed) status=trimmed;
    }
    text("OBJECT_RECLAIM");field("retired",retired);field("free",pages.free_count());field("total",pages.total_count());
    field("objects",pages.object_count());field("users",pages.user_count());field("allocations",pages.allocations());field("releases",pages.releases());put('\n');
    if (status && (pages.used_count() || pages.free_count()!=pages.total_count() || pages.allocations()!=pages.releases()))
        status=Status::fault("page ownership leaked");
    return status;
}
extern "C" void kernel_main()
{
    const auto status=probe();
    text("OBJECT_DONE");field("status",static_cast<u32>(status.code()));text(" scope=actual-user-load-supervisor-object\n");
    for (;;) asm volatile("" ::: "memory");
}
