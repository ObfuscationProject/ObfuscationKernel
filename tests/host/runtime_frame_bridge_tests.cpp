#include "ok/boot/qemu_runtime_frames.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
using namespace ok;
using namespace ok::boot;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"%d: %s\n",__LINE__,#x);std::abort();}}while(false)
void exercise()
{
    auto source=std::make_unique<FirmwareMemory>();
    source->source={arch::Architecture::i386,BootProtocol::bios_e820,FirmwareFormat::e820};
    source->source_storage={0x5000,3088};
    source->e820.count=1;source->e820.records[0]={0,0x100000000ULL,1,1};
    QemuEntryMemoryPlan plan;
    CHECK(qemu_runtime_memory_plan<arch::Architecture::i386>(*source,{0x100000,0x200000},{},1024,plan));
    CHECK(plan.policy.accessible.address==0 && plan.policy.accessible.size==0x40000000 && plan.policy.maximum_ranges==max_qualified_ranges);
    auto workspace=std::make_unique<QualifiedBootMemory>(),qualified=std::make_unique<QualifiedBootMemory>();
    CHECK(qualify_boot_memory(*source,{plan.reservations.begin(),plan.reservations.size()},plan.policy,*workspace,*qualified));
    auto frames=std::make_unique<memory::FrameAllocator>();
    static memory::FrameMetadataStorage<1024, 64> metadata;
    CHECK(qualified_frame_metadata_bytes(*qualified).value()<=metadata.bytes.size());
    CHECK(initialize_qualified_frames(*qualified,*frames,metadata.bytes));
    CHECK(frames->total_frames()==1024 && frames->free_frames()==1024);
    auto first=frames->allocate();CHECK(first && first.value().address==0x500000);
    CHECK(!initialize_qualified_frames(*qualified,*frames,metadata.bytes));CHECK(frames->free_frames()==1023);
    CHECK(frames->release(first.value()));CHECK(!frames->release(first.value()));
    const auto before=plan;
    // The page bound is the caller's policy; allocator metadata is sized from it.
    CHECK(qemu_runtime_memory_plan<arch::Architecture::i386>(*source,{0x100000,0x200000},{},16385,plan));
    CHECK(plan.policy.maximum_pages==16385 && plan.reservations.size()==before.reservations.size());
    plan=before;
    CHECK(!qemu_runtime_memory_plan<arch::Architecture::i386>(*source,{0x100000,0x40000000},{},1024,plan));
    QualifiedBootMemory empty;
    CHECK(!initialize_qualified_frames(empty,*frames,metadata.bytes));CHECK(frames->free_frames()==1024);
    auto policy=plan.policy;policy.page_size=8192;
    CHECK(qualify_boot_memory(*source,{plan.reservations.begin(),plan.reservations.size()},policy,*workspace,*qualified));
    CHECK(!initialize_qualified_frames(*qualified,*frames,metadata.bytes));CHECK(frames->free_frames()==1024);
    policy=plan.policy;policy.maximum_pages=16385;
    CHECK(qualify_boot_memory(*source,{plan.reservations.begin(),plan.reservations.size()},policy,*workspace,*qualified));
    CHECK(!initialize_qualified_frames(*qualified,*frames,metadata.bytes));CHECK(frames->free_frames()==1024);
    source->source={arch::Architecture::ppc,BootProtocol::epapr,FirmwareFormat::fdt};
    source->source.initial_mapping_end=0x20000000;
    source->source_storage={0x1ff00000,0x100000};source->fdt.ram_count=1;source->fdt.ram[0]={0,0x20000000};
    CHECK(qemu_runtime_memory_plan<arch::Architecture::ppc>(*source,{0x1000000,0x100000},{},1024,plan));
    CHECK(plan.policy.accessible.size==0x10000000);
    CHECK(qualify_boot_memory(*source,{plan.reservations.begin(),plan.reservations.size()},plan.policy,*workspace,*qualified));
    CHECK(initialize_qualified_frames(*qualified,*frames,metadata.bytes));
    source->source={arch::Architecture::arm32,BootProtocol::arm32_linux_image,FirmwareFormat::fdt};
    source->source_storage={0x48000000,0x100000};source->fdt.ram_count=1;source->fdt.ram[0]={0x40000000,0x20000000};
    CHECK(qemu_runtime_memory_plan<arch::Architecture::arm32>(*source,{0x40010000,0x300123},{},1024,plan));
    CHECK(plan.policy.accessible.address==0x40000000 && plan.policy.accessible.size==0x10000000);
    CHECK(plan.reservations[0].range.size==0x10000 && plan.reservations[1].range.size==0x300123);
    CHECK(qualify_boot_memory(*source,{plan.reservations.begin(),plan.reservations.size()},plan.policy,*workspace,*qualified));
    CHECK(qualified->page_count()==1024 && qualified->managed()[0].address==0x40311000);
    CHECK(qualified->source().fdt.ram[0].size==0x20000000);
    for(auto range:qualified->managed())CHECK(range.address>=0x40311000 && range.address+range.size<=0x50000000 && !(range.address<0x48100000 && 0x48000000<range.address+range.size));
    CHECK(!qemu_runtime_memory_plan<arch::Architecture::arm32>(*source,{0x40010000,0x10000000},{},1024,plan));
    CHECK(plan.policy.accessible.size==0x10000000);
    source->source.protocol=BootProtocol::arm32_direct_elf;
    CHECK(!qemu_runtime_memory_plan<arch::Architecture::arm32>(*source,{0x40010000,0x300123},{},1024,plan));
    CHECK(!qemu_runtime_memory_plan<arch::Architecture::arm32>(*source,{0x1000000,0x100000},{},1024,plan));
    CHECK(!qemu_runtime_memory_plan<arch::Architecture::loongarch64>(*source,{0x1000000,0x100000},{},1024,plan));
    CHECK(!qemu_runtime_memory_plan<arch::Architecture::mips>(*source,{0x1000000,0x100000},{},1024,plan));
    CHECK(!qemu_runtime_memory_plan<arch::Architecture::mips64>(*source,{0x1000000,0x100000},{},1024,plan));
}
int main(){exercise();std::puts("PASS runtime frame bridge hosted admission, narrowing, capacity and failure preservation; no native dereferences");}
