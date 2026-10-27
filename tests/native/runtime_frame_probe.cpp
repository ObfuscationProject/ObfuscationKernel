#include "ok/boot/qemu_runtime_frames.hpp"
#include "ok/arch/execution.hpp"
#include "ok/boot/raw_handoff.hpp"
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
static void hex(word value){hex64(value);}
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
static const char *reason(ok::boot::DiscoveryFailure value)
{
    using enum ok::boot::DiscoveryFailure;
    switch(value){case none:return "none";case not_captured:return "not_captured";case source_absent:return "source_absent";case unsupported_protocol:return "unsupported_protocol";case protocol_mismatch:return "protocol_mismatch";case unmapped_source:return "unmapped_source";case incomplete_source:return "incomplete_source";case source_capacity_exceeded:return "source_capacity_exceeded";case malformed_source:return "malformed_source";case parser_rejected:return "parser_rejected";case invalid_workspace:return "invalid_workspace";}
    return "invalid";
}
extern "C" char _kernel_start[], _kernel_end[];
extern "C" void ok_platform_console_write_char(char c) { put(c); }
extern "C" const ok::u8 ok_frame_user_begin[], ok_frame_user_end[];
static bool entry_mode()
{
#if defined(OK_ARCH_TARGET_X86_64) || defined(OK_ARCH_TARGET_I386)
    word control; asm volatile("mov %%cr0,%0":"=r"(control));
#ifdef OK_ARCH_TARGET_X86_64
    return (control & (word{1} << 31)) != 0;
#else
    return (control & (word{1} << 31)) == 0;
#endif
#elif defined(OK_ARCH_TARGET_AARCH64)
    word control, level; asm volatile("mrs %0,sctlr_el1":"=r"(control)); asm volatile("mrs %0,CurrentEL":"=r"(level));
    return !(control & 1) && level == 4;
#elif defined(OK_ARCH_TARGET_RV32) || defined(OK_ARCH_TARGET_RV64)
    word satp, status; asm volatile("csrr %0,satp":"=r"(satp)); asm volatile("csrr %0,mstatus":"=r"(status));
    return satp == 0 && !(status & (word{1} << 17));
#else
    return true; // ePAPR initial mapping is checked through the captured contract.
#endif
}
static void memory_probe(const ok::boot::FirmwareMemory &source, bool discovered)
{
    using namespace ok;using namespace ok::boot;
    const u64 first = reinterpret_cast<uptr>(_kernel_start), last = reinterpret_cast<uptr>(_kernel_end);
    text("MEMORY_IMAGE base=");hex64(first);text(" size=");hex64(last-first);put('\n');
    QemuEntryMemoryPlan plan;
    static QualifiedBootMemory workspace, memory;
    auto status = discovered ? qemu_runtime_memory_plan<arch::Architecture::OK_PROBE_ARCH>(source,{first,last-first},{},1024,plan)
                             : Status::unsupported("no firmware map");
    if (status && !entry_mode()) status=Status::fault("unexpected entry translation state");
    if (status) status=qualify_boot_memory(source,{plan.reservations.begin(),plan.reservations.size()},plan.policy,workspace,memory);
    static ok::memory::FrameAllocator frames;
    static ok::memory::FrameMetadataStorage<1024, 64> frame_metadata;
    static std::array<ok::memory::PhysicalFrame, 1024> handles;
    bool native_pass = false;
    if (status)
    {
        status = arch::execution::initialize();
        if (status) status = initialize_qualified_frames(memory, frames, frame_metadata.bytes);
        if (status && frames.total_frames() != 1024) status = Status::invalid_argument("frame count differs");
        usize allocated = 0;
        for (; status && allocated < handles.size(); ++allocated)
        {
            auto frame = frames.allocate();
            if (!frame) { status=frame.status(); break; }
            handles[allocated]=frame.value();
        }
        if (status && (frames.free_frames() != 0 || frames.allocate())) status=Status::io_error("pressure admitted an extra frame");
        if (status)
        {
            namespace hw=arch::execution;
            const usize code_size=static_cast<usize>(ok_frame_user_end-ok_frame_user_begin);
            if (!code_size || code_size > hw::page_size) status=Status::invalid_argument("user program size");
            else
            {
                for (usize i=0;i<3;++i) memset(reinterpret_cast<void *>(handles[i].address),0,4096);
                memcpy(reinterpret_cast<void *>(handles[0].address),ok_frame_user_begin,code_size);
                const std::array<hw::PageMapping,3> maps{{
                    {hw::user_begin, handles[0].address, hw::page_read|hw::page_execute},
                    {hw::user_begin+4096, handles[1].address, hw::page_read|hw::page_write},
                    {hw::user_end-4096, handles[2].address, hw::page_read|hw::page_write}}};
                auto space=hw::create_space(maps);
                if (!space) status=space.status();
                else
                {
                    auto frame=hw::initial_frame(hw::user_begin,hw::user_end-16);
                    bool trapped=false;
                    for(usize attempt=0;status && attempt<128;++attempt)
                    {
                        auto trap=hw::enter(space.value(),frame);
                        if (!trap) { status=trap.status();break; }
                        if (trap.value().kind==hw::TrapKind::timer) continue;
                        trapped=trap.value().kind==hw::TrapKind::syscall;
                        break;
                    }
                    u32 value=0;
                    if (!trapped || !hw::copy_from_user(space.value(),&value,hw::user_begin+4096,sizeof(value)) || value!=0x12345678)
                        status=Status::io_error("native frame store did not execute");
                    auto retired=hw::destroy_space(space.value());
                    if (!retired) { status=retired;allocated=0; } // Quarantine every frame if translations survive.
                }
            }
        }
        const auto stale=handles[0];
        for (usize i=0;i<allocated;++i) if (auto released=frames.release(handles[i]);!released && status) status=released;
        if (status && frames.free_frames()!=1024) status=Status::io_error("frame reclamation failed");
        if (status)
        {
            if(frames.release(stale)) status=Status::io_error("double release accepted");
            auto fresh=frames.allocate();
            if (!fresh || fresh.value().generation==stale.generation || frames.release(stale)) status=Status::io_error("stale generation accepted");
            if(fresh) { auto released=frames.release(fresh.value());if(!released)status=released; }
        }
        native_pass=status.ok();
    }
    text("FRAME_NATIVE status=");hex64(static_cast<u32>(status.code()));text(" executed=");hex64(native_pass);text(" free=");hex64(frames.free_frames());text(" total=");hex64(frames.total_frames());put('\n');
    u64 touched=0;
    if (status)
    {
        text("MEMORY_POLICY base=");hex64(plan.policy.accessible.address);text(" size=");hex64(plan.policy.accessible.size);text(" ceiling=");hex64(plan.policy.address_ceiling);put('\n');
        for (const auto object : memory.loaded()) {text("MEMORY_LOADED base=");hex64(object.range.address);text(" size=");hex64(object.range.size);text(" ram=");hex64(object.require_usable_ram);put('\n');}
        for (const auto range : memory.managed())
        {
            text("MEMORY_MANAGED base=");hex64(range.address);text(" size=");hex64(range.size);put('\n');
            for (u64 address=range.address;address<range.address+range.size;address+=4096)
            {
                // Qualification is exercised with reversible actual RAM writes;
                // this is not an allocator, user mapping, DMA or SMP test.
                auto *begin=reinterpret_cast<volatile u32 *>(static_cast<uptr>(address));
                auto *end=reinterpret_cast<volatile u32 *>(static_cast<uptr>(address+4092));
                const u32 old_begin=*begin, old_end=*end;
                const u32 pattern=0xa5190000u ^ static_cast<u32>(address>>12);
                *begin=pattern;*end=~pattern;
                const bool correct=*begin==pattern && *end==~pattern;
                *begin=old_begin;*end=old_end;
                if (!correct) status=Status::io_error("physical page pattern mismatch");
                ++touched;
            }
        }
        for(const auto exclusion:memory.excluded()) {text("MEMORY_EXCLUDED base=");hex64(exclusion.range.address);text(" size=");hex64(exclusion.range.size);text(" reason=");hex64(static_cast<u8>(exclusion.reason));put('\n');}
    }
    text("MEMORY_RESULT status=");hex64(static_cast<u32>(status.code()));text(" pages=");hex64(memory.page_count());text(" touched=");hex64(touched);text(" process_allocator=unchanged\n");
}
extern "C" void kernel_main()
{
    using namespace ok;using namespace ok::boot;
    const auto &raw=raw_boot_handoff();
#if defined(OK_ARCH_TARGET_ARM32)
    const auto &entry=arm32_entry_state();
    text("ARM32_ENTRY cpsr=");hex64(entry.cpsr);text(" sctlr=");hex64(entry.sctlr);text(" mpidr=");hex64(entry.mpidr);text(" valid=");hex64(valid_arm32_entry(entry));put('\n');
#endif
    text("FIRMWARE_RAW captured=");hex(raw.captured);text(" values=");for(auto v:raw.arguments){hex64(v);put(' ');}put('\n');
    static FirmwareMemory scratch,output;
    output.source_storage={123,456};
    ProbeReader reader;
    const auto result=discover_firmware_memory<arch::Architecture::OK_PROBE_ARCH>(raw,BootProtocol::OK_PROBE_PROTOCOL,reader,scratch,output);
    if(result)
    {
        text("FIRMWARE_STORAGE base=");hex64(output.source_storage.address);text(" size=");hex64(output.source_storage.size);text(" decoded=");hex64(output.decoded_bytes);put('\n');
        if(output.source.format==FirmwareFormat::e820)
        {
            text("E820_MAP count=");hex64(output.e820.count);put('\n');
            for(usize i=0;i<output.e820.count;++i){const auto&r=output.e820.records[i];text("E820_RECORD base=");hex64(r.address);text(" size=");hex64(r.size);text(" type=");hex64(r.type);text(" attributes=");hex64(r.attributes);put('\n');}
        }
        else
        {
            text("FDT_MAP ram=");hex64(output.fdt.ram_count);text(" reservations=");hex64(output.fdt.reservation_count);text(" boot_cpu=");hex64(output.fdt.boot_cpu_id);put('\n');
            for(usize i=0;i<output.fdt.ram_count;++i){const auto&r=output.fdt.ram[i];text("FDT_RAM base=");hex64(r.address);text(" size=");hex64(r.size);put('\n');}
            for(usize i=0;i<output.fdt.reservation_count;++i){const auto&r=output.fdt.reservations[i];text("FDT_RESERVED base=");hex64(r.range.address);text(" size=");hex64(r.range.size);text(" source=");hex64(static_cast<u8>(r.source));text(" no_map=");hex64(r.no_map);text(" reusable=");hex64(r.reusable);put('\n');}
        }
    }
    else if(output.source_storage.address!=123||output.source_storage.size!=456)text("FIRMWARE_FAIL output-mutated\n");
    text("FIRMWARE_RESULT reason=");text(reason(result.failure));text(" status=");hex64(static_cast<u32>(result.status.code()));text(" reads=");hex64(reader.calls);text(" allocator=unqualified\n");
    memory_probe(output,static_cast<bool>(result));
    text("FIRMWARE_DONE scope=actual-qemu-boot\n");
    for(;;)asm volatile("":::"memory");
}
