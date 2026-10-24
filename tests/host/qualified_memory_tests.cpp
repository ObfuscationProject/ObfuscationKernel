#include "ok/boot/qemu_entry_memory.hpp"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <vector>
using namespace ok;
using namespace ok::boot;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"%d: %s\n",__LINE__,#x); std::abort(); } } while (0)

FirmwareMemory map()
{
    FirmwareMemory source;
    source.source.format = FirmwareFormat::e820;
    source.source_storage = {0x5000, 0xc10};
    source.e820.records[0] = {0, 0x200000, 1, 1};
    source.e820.records[1] = {0x100000, 0x200000, 1, 1}; // Overlapping usable RAM.
    source.e820.records[2] = {0x180011, 2, 2, 1}; // A sub-page reservation.
    source.e820.records[3] = {0x100000000, 0x200000, 1, 1};
    source.e820.count = 4;
    return source;
}
ManagedMemoryPolicy policy()
{
    return {{0, 0x200000000}, 0x100000000, 4096, 16384, ManagedSelection::require_all};
}
bool owns(const QualifiedBootMemory &memory, u64 page)
{
    for (auto range : memory.managed())
        if (page >= range.address && page - range.address < range.size) return true;
    return false;
}
template<arch::Architecture A>
void entry_policy_test()
{
    using enum arch::Architecture;
    FirmwareMemory source;
    source.source.architecture = A;
    source.source.format = (A == i386 || A == x86_64) ? FirmwareFormat::e820 : FirmwareFormat::fdt;
    source.source.protocol = (A == i386 || A == x86_64) ? BootProtocol::bios_e820 :
        A == arm32 ? BootProtocol::arm32_linux_image : A == aarch64 ? BootProtocol::aarch64_linux_image : A == ppc ? BootProtocol::epapr : BootProtocol::riscv_fdt;
    source.source.initial_mapping_end = 0x4000000;
    constexpr u64 image_start = (A == i386 || A == x86_64) ? 0x100000 : A == arm32 ? 0x40010000 : A == aarch64 ? 0x40080000 : A == ppc ? 0x1000000 : 0x80000000;
    QemuEntryMemoryPlan plan;
    const auto result = qemu_entry_memory_plan<A>(source,{image_start,0x180000},{},1024,plan);
    if constexpr (A == loongarch64 || A == mips || A == mips64) {
        CHECK(!result && result.code() == StatusCode::unsupported);
    } else {
        CHECK(result && plan.policy.maximum_pages == 1024);
        CHECK(plan.reservations.size() == (A == rv32 || A == rv64 ? 1 : 2));
        CHECK(plan.reservations[plan.reservations.size()-1].range.address == image_start);
        CHECK(!qemu_entry_memory_plan<A>(source,{image_start+1,0x180000},{},1024,plan));
        CHECK(!qemu_entry_memory_plan<A>(source,{image_start,0x180000},{},0,plan));
        CHECK(plan.policy.maximum_pages == 1024);
        if constexpr (A == x86_64) CHECK(plan.policy.accessible.size == 0x40000000);
        if constexpr (A == ppc) CHECK(plan.policy.accessible.size == source.source.initial_mapping_end);
        source.source.protocol = BootProtocol::unknown;
        CHECK(!qemu_entry_memory_plan<A>(source,{image_start,0x180000},{},1024,plan));
        CHECK(plan.policy.maximum_pages == 1024);
    }
}
int main()
{
    entry_policy_test<arch::Architecture::i386>(); entry_policy_test<arch::Architecture::x86_64>();
    entry_policy_test<arch::Architecture::aarch64>(); entry_policy_test<arch::Architecture::arm32>();
    entry_policy_test<arch::Architecture::rv64>(); entry_policy_test<arch::Architecture::rv32>();
    entry_policy_test<arch::Architecture::loongarch64>(); entry_policy_test<arch::Architecture::mips>();
    entry_policy_test<arch::Architecture::mips64>(); entry_policy_test<arch::Architecture::ppc>();
    static QualifiedBootMemory output, workspace;
    auto source = map();
    std::array<LoadedReservation,2> loaded{{{{0,0x10000},false},{{0x80001,1},true}}};
    auto settings = policy();
    CHECK(qualify_boot_memory(source,loaded,settings,workspace,output));
    CHECK(output.valid() && output.source().e820.count == 4);
    CHECK(output.source().e820.records[3].address == 0x100000000);
    CHECK(!owns(output,0) && !owns(output,0x5000) && !owns(output,0x80000));
    CHECK(!owns(output,0x180000) && owns(output,0x181000));
    CHECK(!owns(output,0x100000000));
    CHECK(output.page_count() == (0x300000-0x10000)/4096-2);
    u64 previous_end = 0;
    for (auto range : output.managed()) {
        CHECK(range.address >= previous_end && !(range.address & 4095) && !(range.size & 4095));
        previous_end = range.address+range.size;
    }
    bool high = false;
    for (auto excluded : output.excluded())
        high |= excluded.range.address == 0x100000000 && excluded.reason == ExclusionReason::address_width;
    CHECK(high);
    const auto saved_count = output.page_count();
    settings.maximum_pages = 3;
    CHECK(!qualify_boot_memory(source,loaded,settings,workspace,output));
    CHECK(output.valid() && output.page_count() == saved_count);
    settings.selection = ManagedSelection::lowest_addresses;
    CHECK(qualify_boot_memory(source,loaded,settings,workspace,output));
    CHECK(output.page_count() == 3 && output.managed().size() == 1 && output.managed()[0].address == 0x10000);
    settings = policy(); settings.maximum_ranges = 1;
    CHECK(!qualify_boot_memory(source,loaded,settings,workspace,output));
    CHECK(output.page_count() == 3);
    settings.selection = ManagedSelection::lowest_addresses;
    CHECK(qualify_boot_memory(source,loaded,settings,workspace,output));
    CHECK(output.managed().size() == 1 && output.policy().maximum_ranges == 1);
    settings = policy(); settings.accessible = {0x100000,0x10000};
    CHECK(qualify_boot_memory(source,loaded,settings,workspace,output));
    CHECK(output.page_count() == 16);
    const auto saved_start = output.managed()[0].address;
    loaded[0].require_usable_ram = true;
    loaded[0].range = {0x400000,4096};
    CHECK(!qualify_boot_memory(source,loaded,settings,workspace,output));
    CHECK(output.managed()[0].address == saved_start && output.page_count() == 16);
    loaded[0].range = {0x180000,4096};
    CHECK(!qualify_boot_memory(source,loaded,settings,workspace,output));
    CHECK(!qualify_boot_memory(source,{},settings,output,output));
    CHECK(!qualify_boot_memory(workspace.source(),{},settings,workspace,output));
    settings = policy(); source.e820.records[0].size = ~u64{0}; source.e820.records[0].address = 1;
    CHECK(!qualify_boot_memory(source,{},settings,workspace,output));
    // Nested loaded extents reserve their union, without creating a firmware conflict.
    source = map(); settings = policy();
    loaded = {{{{0x100000,0x10000},true},{{0x101000,0x1000},true}}};
    CHECK(qualify_boot_memory(source,loaded,settings,workspace,output));
    CHECK(output.loaded().size() == 2 && !owns(output,0x101000));
    CHECK(!qualify_boot_memory(source,workspace.loaded(),settings,workspace,output));
    CHECK(!qualify_boot_memory(source,{},settings,workspace,workspace));
    CHECK(!qualify_boot_memory(output.source(),{},settings,workspace,output));
    // Output-backed reservations are stable until final publication.
    CHECK(qualify_boot_memory(source,output.loaded(),settings,workspace,output));
    const auto retained = output.page_count();
    loaded[0].range = source.source_storage;
    CHECK(!qualify_boot_memory(source,loaded,settings,workspace,output));
    CHECK(output.page_count() == retained);
    // A complete valid final partial page is excluded, without alignment overflow.
    source = {}; source.source.format = FirmwareFormat::fdt;
    source.source_storage = {0x100,16}; source.fdt.ram_count = 2;
    source.fdt.ram[0] = {0x1000,4096}; source.fdt.ram[1] = {~u64{0}-2,2};
    settings = {{0,~u64{0}},~u64{0},4096,1,ManagedSelection::require_all};
    CHECK(qualify_boot_memory(source,{},settings,workspace,output));
    CHECK(output.page_count() == 1 && output.excluded().size() == 1);
    CHECK(output.excluded()[0].range.address == ~u64{0}-2);
    settings.maximum_pages = ~u64{0};
    CHECK(!qualify_boot_memory(source,{},settings,workspace,output));
    CHECK(output.page_count() == 1);
    settings.maximum_pages = 1; settings.page_size = 3;
    CHECK(!qualify_boot_memory(source,{},settings,workspace,output));
    source.fdt.ram_count = source.fdt.ram.size()+1;
    settings.page_size = 4096;
    CHECK(!qualify_boot_memory(source,{},settings,workspace,output));
    // Exactly 512 exclusion records fit; a further capacity exclusion must fail
    // atomically, even when lowest-address subset selection was requested.
    source = {}; source.source.format=FirmwareFormat::fdt;
    source.source_storage={0xf0000000,4096};
    source.fdt.ram_count=64;source.fdt.reservation_count=128;
    for (usize i=0;i<64;++i) {
        const u64 base=i*0x10000;
        source.fdt.ram[i]={base+1,12*4096-2};
        source.fdt.reservations[2*i].range={base+2*4096+1,4096-2};
        source.fdt.reservations[2*i+1].range={base+6*4096+1,4096-2};
    }
    settings={{0,0x100000000ULL},0x100000000ULL,4096,4096,ManagedSelection::require_all};
    CHECK(qualify_boot_memory(source,{},settings,workspace,output));
    CHECK(output.excluded().size()==512 && output.managed().size()==192);
    const auto full_count=output.page_count();
    settings.maximum_pages=1;settings.selection=ManagedSelection::lowest_addresses;
    CHECK(!qualify_boot_memory(source,{},settings,workspace,output));
    CHECK(output.page_count()==full_count && output.excluded().size()==512);
    std::array<LoadedReservation,max_loaded_reservations+1> too_many{};
    CHECK(!qualify_boot_memory(source,too_many,settings,workspace,output));
    // Deterministic randomized maps, checked against a page-by-page oracle.
    u32 random = 0x9361831;
    for (unsigned trial=0; trial<1000; ++trial) {
        auto next=[&] {random=random*1664525u+1013904223u;return random;};
        source={};source.source.format=FirmwareFormat::fdt;
        source.source_storage={0x10000,123};source.fdt.ram_count=4;source.fdt.reservation_count=7;
        std::array<bool,64> usable{},reserved{};
        for (usize i=0;i<source.fdt.ram_count;++i) {
            u64 first=next()%56, count=1+next()%8;source.fdt.ram[i]={first*4096,count*4096};
            for (u64 page=first;page<first+count;++page) usable[page]=true;
        }
        for (usize i=0;i<source.fdt.reservation_count;++i) {
            u64 first=next()%60, count=1+next()%4;source.fdt.reservations[i].range={first*4096+3,count*4096-7};
            for (u64 page=first;page<first+count;++page) reserved[page]=true;
        }
        reserved[16]=true;
        settings={{0,64*4096},64*4096,4096,64,ManagedSelection::require_all};
        u64 expected=0;for(usize i=0;i<64;++i) expected+=usable[i]&&!reserved[i];
        const auto status=qualify_boot_memory(source,{},settings,workspace,output);
        CHECK(bool(status)==(expected!=0));
        if(status) {
            CHECK(output.page_count()==expected);
            for(usize i=0;i<64;++i) CHECK(owns(output,i*4096)==(usable[i]&&!reserved[i]));
            // Byte conservation catches duplicated, missing or overlapping
            // exclusions that a managed-page-only oracle cannot detect.
            std::vector<u64> boundaries{0,64*4096};
            for(usize i=0;i<source.fdt.ram_count;++i) {const auto r=source.fdt.ram[i];boundaries.push_back(r.address);boundaries.push_back(r.address+r.size);}
            for(const auto r:output.managed()) {boundaries.push_back(r.address);boundaries.push_back(r.address+r.size);}
            for(const auto e:output.excluded()) {boundaries.push_back(e.range.address);boundaries.push_back(e.range.address+e.range.size);}
            std::sort(boundaries.begin(),boundaries.end());
            for(const u64 byte:boundaries) {
                if(byte==64*4096)continue;
                usize count=0;
                for(const auto range:output.managed()) count+=byte>=range.address && byte-range.address<range.size;
                for(const auto exclusion:output.excluded()) count+=byte>=exclusion.range.address && byte-exclusion.range.address<exclusion.range.size;
                CHECK(count==usize(usable[byte/4096]));
            }
        }
    }
    std::puts("PASS 1000 normalization oracle cases plus capacity/reservation/u64/failure-atomicity checks (hosted only)");
}
