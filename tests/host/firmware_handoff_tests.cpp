#include "ok/boot/firmware_handoff.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace ok;
using namespace ok::boot;
using enum arch::Architecture;
using Bytes = std::vector<std::byte>;
static void set32(Bytes &b, usize p, u32 value, bool be = false)
{
    for (usize i = 0; i < 4; ++i) b[p+i] = std::byte(value >> ((be ? 3-i : i)*8));
}
static void append(Bytes &b, u32 value) { const auto n=b.size(); b.resize(n+4); set32(b,n,value,true); }
static void label(Bytes &b, const char *s)
{
    do { b.push_back(std::byte(*s)); } while (*s++);
    while (b.size()%4) b.push_back(std::byte{});
}
static Bytes fdt_blob()
{
    Bytes b(56), names;
    auto begin=[&](const char *s){append(b,1);label(b,s);};
    auto prop=[&](const char *name, std::initializer_list<u32> cells){
        append(b,3);append(b,static_cast<u32>(cells.size()*4));append(b,static_cast<u32>(names.size()));
        do{names.push_back(std::byte(*name));}while(*name++);
        for(auto cell:cells)append(b,cell);
    };
    begin("");prop("#address-cells",{2});prop("#size-cells",{2});
    begin("memory@100000000");prop("reg",{1,0,0,0x200000});append(b,2);
    begin("reserved-memory");prop("#address-cells",{2});prop("#size-cells",{2});prop("ranges",{});
    begin("reserved@100001000");prop("reg",{1,0x1000,0,0x1000});prop("no-map",{});append(b,2);
    append(b,2);append(b,2);append(b,9);
    const auto strings=b.size();b.insert(b.end(),names.begin(),names.end());
    set32(b,0,0xd00dfeed,true);set32(b,4,static_cast<u32>(b.size()),true);
    set32(b,8,56,true);set32(b,12,static_cast<u32>(strings),true);set32(b,16,40,true);
    set32(b,20,17,true);set32(b,24,16,true);set32(b,32,static_cast<u32>(names.size()),true);
    set32(b,36,static_cast<u32>(strings-56),true);return b;
}
static Bytes e820_blob()
{
    Bytes b(16+3*24);set32(b,0,e820_handoff_magic);set32(b,4,1);set32(b,8,3);set32(b,12,24);
    set32(b,16,0);set32(b,20,1);set32(b,24,0x200000);set32(b,32,1);set32(b,36,1);
    set32(b,40,0x1000);set32(b,44,1);set32(b,48,0x1000);set32(b,56,77);set32(b,60,5);
    set32(b,64,0);set32(b,68,2);set32(b,72,0x1000);set32(b,80,1);set32(b,84,0);
    return b;
}
struct Reader
{
    u64 address{};Bytes bytes;usize calls{};usize reject_call{};int bad_size{};bool mutate{};
    Result<std::span<const std::byte>> read_physical(u64 a,usize count)
    {
        ++calls;
        if(calls==reject_call||a!=address||count>bytes.size())return Status::fault("unmapped test input");
        if(mutate&&calls==2)set32(bytes,8,1);
        return std::span<const std::byte>{bytes.data(),count+bad_size};
    }
};
static_assert(FirmwareReader<Reader>);
static RawBootHandoff raw(uptr a, uptr b=0,uptr c=0,uptr d=0){return {{a,b,c,d},true};}
template<arch::Architecture A>
static void reject(RawBootHandoff input,BootProtocol protocol,Reader &reader,DiscoveryFailure reason,usize calls)
{
    FirmwareMemory workspace{},out{};out.source_storage={123,456};out.e820.count=27;
    const auto old=out;
    const auto result=discover_firmware_memory<A>(input,protocol,reader,workspace,out);
    assert(!result&&result.failure==reason&&reader.calls==calls);
    assert(std::memcmp(&out,&old,sizeof(out))==0);
}
int main()
{
    FirmwareMemory workspace{},output{};
    Reader e820{0x5000,e820_blob()};
    assert(discover_firmware_memory<i386>(raw(e820_handoff_magic,0x5000),BootProtocol::bios_e820,e820,workspace,output));
    assert(e820.calls==2&&output.e820.count==3&&output.source_storage.size==3088&&output.decoded_bytes==88);
    assert(output.e820.records[0].address==0x100000000ULL&&output.e820.records[0].usable());
    assert(output.e820.records[1].type==77&&output.e820.records[1].attributes==5);
    assert(!output.e820.records[2].usable());
    const auto saved=output;
    e820.bytes.clear();assert(output.e820.records[0].address==0x100000000ULL);
    auto alias=discover_firmware_memory<x86_64>(raw(e820_handoff_magic,0x5000),BootProtocol::bios_e820,e820,output,output);
    assert(!alias&&alias.failure==DiscoveryFailure::invalid_workspace&&std::memcmp(&output,&saved,sizeof(output))==0);
    for(const auto &pair:{std::pair{2u,DiscoveryFailure::incomplete_source},std::pair{3u,DiscoveryFailure::source_capacity_exceeded},std::pair{9u,DiscoveryFailure::malformed_source}})
    {
        Reader r{0x5000,e820_blob()};set32(r.bytes,4,pair.first);
        reject<x86_64>(raw(e820_handoff_magic,0x5000),BootProtocol::bios_e820,r,pair.second,1);
    }
    for(usize nth:{1u,2u}){Reader r{0x5000,e820_blob(),0,nth};reject<x86_64>(raw(e820_handoff_magic,0x5000),BootProtocol::bios_e820,r,DiscoveryFailure::unmapped_source,nth);}
    for(int bad:{-1,1}){Reader r{0x5000,e820_blob(),0,0,bad};reject<x86_64>(raw(e820_handoff_magic,0x5000),BootProtocol::bios_e820,r,DiscoveryFailure::unmapped_source,1);}
    {Reader r{0x5000,e820_blob()};r.mutate=true;reject<x86_64>(raw(e820_handoff_magic,0x5000),BootProtocol::bios_e820,r,DiscoveryFailure::malformed_source,2);}
    {Reader r{0x5000,e820_blob()};set32(r.bytes,8,129);reject<i386>(raw(e820_handoff_magic,0x5000),BootProtocol::bios_e820,r,DiscoveryFailure::source_capacity_exceeded,1);}
    {Reader r{0x5000,e820_blob()};set32(r.bytes,16,0xfffff000);set32(r.bytes,20,0xffffffff);reject<i386>(raw(e820_handoff_magic,0x5000),BootProtocol::bios_e820,r,DiscoveryFailure::parser_rejected,2);}
    {Reader r{};reject<i386>({},BootProtocol::bios_e820,r,DiscoveryFailure::not_captured,0);reject<x86_64>(raw(0),BootProtocol::bios_e820,r,DiscoveryFailure::source_absent,0);reject<x86_64>(raw(0x2badb002,0x5000),BootProtocol::bios_e820,r,DiscoveryFailure::protocol_mismatch,0);reject<x86_64>(raw(e820_handoff_magic,0x5008),BootProtocol::bios_e820,r,DiscoveryFailure::protocol_mismatch,0);}
    const auto blob=fdt_blob();
    {Reader r{0x48000000,blob};assert(discover_firmware_memory<aarch64>(raw(0x48000000),BootProtocol::aarch64_linux_image,r,workspace,output));assert(output.fdt.ram_count==1&&output.fdt.ram[0].address==0x100000000ULL);assert(output.fdt.reservation_count==1&&output.fdt.reservations[0].no_map);}
    {Reader r{0x9fe00000,blob};assert(discover_firmware_memory<rv64>(raw(7,0x9fe00000,0x1028),BootProtocol::riscv_fdt,r,workspace,output));assert(output.source.arguments[0]==7&&output.source.arguments[2]==0x1028);}
    {Reader r{0x9fe00000,blob};assert(discover_firmware_memory<rv32>(raw(0,0x9fe00000),BootProtocol::riscv_fdt,r,workspace,output));assert(output.fdt.ram[0].address==0x100000000ULL);}
    {Reader r{0x2900000,blob};assert(discover_firmware_memory<ppc>(raw(0x2900000,0x45504150,0x4000000),BootProtocol::epapr,r,workspace,output));}
    {Reader r{0x2900000,blob};reject<ppc>(raw(0x2900000,0x45504150,0x2900010),BootProtocol::epapr,r,DiscoveryFailure::unmapped_source,0);}
    {Reader r{0x2900000,blob};reject<ppc>(raw(0x2900000,0x45504150,0x2900040),BootProtocol::epapr,r,DiscoveryFailure::unmapped_source,1);}
    {Reader r{0x48000000,blob};set32(r.bytes,4,max_fdt_bytes+1,true);reject<aarch64>(raw(0x48000000),BootProtocol::aarch64_linux_image,r,DiscoveryFailure::source_capacity_exceeded,1);}
    {Reader r{0x48000000,blob};set32(r.bytes,20,16,true);reject<aarch64>(raw(0x48000000),BootProtocol::aarch64_linux_image,r,DiscoveryFailure::parser_rejected,2);}
    {Reader r{};reject<aarch64>(raw(0x48000000,1),BootProtocol::aarch64_linux_image,r,DiscoveryFailure::protocol_mismatch,0);reject<aarch64>(raw(0x48000001),BootProtocol::aarch64_linux_image,r,DiscoveryFailure::protocol_mismatch,0);reject<rv64>(raw(0,~uptr{7}),BootProtocol::riscv_fdt,r,DiscoveryFailure::unmapped_source,0);reject<arm32>(raw(0),BootProtocol::arm32_direct_elf,r,DiscoveryFailure::source_absent,0);reject<loongarch64>(raw(0),BootProtocol::loongarch64_direct_elf,r,DiscoveryFailure::source_absent,0);reject<aarch64>(raw(e820_handoff_magic,0x5000),BootProtocol::bios_e820,r,DiscoveryFailure::unsupported_protocol,0);reject<aarch64>(raw(2,0x80002000,0x80002008,0x10000000),BootProtocol::malta_prom,r,DiscoveryFailure::unsupported_protocol,0);reject<mips>(raw(1,0,0x200),BootProtocol::loongarch64_efi_fdt,r,DiscoveryFailure::unsupported_protocol,0);}
    // Malta and LoongArch loader contracts: an unreadable PROM or system table is unmapped.
    {Reader r{};reject<mips>(raw(2,0x80002000,0x80002008,0x10000000),BootProtocol::malta_prom,r,DiscoveryFailure::unmapped_source,1);}
    {Reader r{};reject<mips64>(raw(2,~uptr{0x7fffdfff},~uptr{0x7fffdff7},0x10000000),BootProtocol::malta_prom,r,DiscoveryFailure::unmapped_source,1);}
    {Reader r{};reject<loongarch64>(raw(1,0,0x200),BootProtocol::loongarch64_efi_fdt,r,DiscoveryFailure::unmapped_source,1);}
    {Reader r{0x48000000,blob};assert(discover_firmware_memory<arm32>(raw(0,0xffffffff,0x48000000,37),BootProtocol::arm32_linux_image,r,workspace,output));assert(r.calls==2&&output.source.arguments[3]==37&&output.source_storage.address==0x48000000&&output.source_storage.size==blob.size());}
    for(auto input:{raw(1,0xffffffff,0x48000000),raw(0,7,0x48000000),raw(0,0xffffffff,0x48000001),raw(0,0xffffffff,0x148000000ULL)})
    {Reader r{};reject<arm32>(input,BootProtocol::arm32_linux_image,r,DiscoveryFailure::protocol_mismatch,0);}
    {Reader r{};reject<arm32>(raw(0,0xffffffff),BootProtocol::arm32_linux_image,r,DiscoveryFailure::source_absent,0);reject<arm32>(raw(0),BootProtocol::arm32_linux_image,r,DiscoveryFailure::protocol_mismatch,0);}
    for(usize nth:{1u,2u}){Reader r{0x48000000,blob,0,nth};reject<arm32>(raw(0,0xffffffff,0x48000000),BootProtocol::arm32_linux_image,r,DiscoveryFailure::unmapped_source,nth);}
    {Reader r{0x48000000,blob};r.mutate=true;reject<arm32>(raw(0,0xffffffff,0x48000000),BootProtocol::arm32_linux_image,r,DiscoveryFailure::malformed_source,2);}
    assert(valid_arm32_entry({0x1d3,0x00c50078,0x80000000,true}));
    for(auto state:{Arm32EntryState{},Arm32EntryState{0x213,0,0,true},Arm32EntryState{0x13,0x02000000,0,true},Arm32EntryState{0x1da,0,0,true},Arm32EntryState{0x33,0,0,true},Arm32EntryState{0x13,1,0,true},Arm32EntryState{0x13,4,0,true},Arm32EntryState{0x13,0,1,true},Arm32EntryState{0x13,0,0x100,true}})assert(!valid_arm32_entry(state));
    std::puts("PASS hosted typed firmware discovery: all-ten source states, bounded reads, owned full-width records, failure atomicity; allocator not qualified");
}
