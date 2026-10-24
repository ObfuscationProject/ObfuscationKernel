#include "ok/boot/firmware_handoff.hpp"
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
    text("FIRMWARE_DONE scope=actual-qemu-boot\n");
    for(;;)asm volatile("":::"memory");
}
