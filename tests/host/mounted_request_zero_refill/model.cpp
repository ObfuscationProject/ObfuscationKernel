// Image helpers only; inherited main and field-comparison controls never run.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main retained_birth_main
#include "../completed_ownership_birth/model.cpp"
#undef main
#pragma GCC diagnostic pop
#include "ext4_realtime_model.hpp"
#undef CHECK
#include "../ext4_owned_typed_zero_large_fixture.hpp"
#include <map>
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "typed request:%d: %s\n", __LINE__, #x); std::abort(); } } while (false)

namespace
{
using Storage = Mount::TypedMutationStorage;
using Kind = Mount::TypedRequestKind;
using Phase = Mount::TypedRequestPhase;
usize successful{}, refused{}, noops{}, allocations{}, truncated{}, sticky{}, maximum_epochs{}, counter_refusals{}, scalar_checks{};
struct RequestDevice final : driver::BlockDevice
{
    std::array<Block, 8705> &blocks;
    const u64 origin{37};
    usize reads{}, writes{}, flushes{};
    explicit RequestDevice(std::array<Block, 8705> &value) : blocks(value) {}
    driver::BlockGeometry geometry() const override { return {origin + blocks.size() * 2, 512, true}; }
    Status read_blocks(u64 sector, std::span<std::byte> output) override
    {
        ++reads; CHECK(sector >= origin && (sector - origin) % 2 == 0 && output.size() == 1024);
        const auto block = (sector - origin) / 2; CHECK(block < blocks.size());
        for (usize at = 0; at < output.size(); ++at) output[at] = std::byte(blocks[block][at]);
        return {};
    }
    Status write_blocks(u64 sector, std::span<const std::byte> input) override
    {
        ++writes; CHECK(sector >= origin && (sector - origin) % 2 == 0 && input.size() == 1024);
        const auto block = (sector - origin) / 2; CHECK(block < blocks.size());
        for (usize at = 0; at < input.size(); ++at) blocks[block][at] = std::to_integer<u8>(input[at]);
        return {};
    }
    Status flush() override { ++flushes; return {}; }
};
void clock_ready()
{
    testing::ext4_clock::reset();
    testing::ext4_clock::set({{1700000333, 123456789}, {1700000333000000000ULL, 1}, 1,
        time::RealtimeOrigin::administrator_software});
}
void head(Image &image, u32 sequence, u32 features)
{
    auto &block=image.blocks[1000];
    be32(block,24,sequence); be32(block,40,features); be32(block,252,0);
    be32(block,252,jbd2::detail::crc32c(~u32{0},block));
}
struct RequestFixture final
{
    Image image;
    RequestDevice device;
    Owner::Registry registry;
    std::unique_ptr<Mount> mount{new Mount(Ext4MountedPolicy::ordered_data_dirty_noatime)};
    std::unique_ptr<Workspace> workspace{new Workspace};
    std::unique_ptr<Storage> storage{new Storage(*mount)};
    Mount::MountedNode *node{};
    RequestFixture(bool cross, u32 count, u32 sequence=70, u32 selected_features=jbd2::feature_checksum_v3)
        : image(count,cross),device(image.blocks)
    {
        media(image,false,false,0,count); head(image,sequence,jbd2::feature_checksum_v3);
        const auto key=registry.register_device(device,31000+fixtures); CHECK(key);
        CHECK(mount->mount(registry,key.value(),{device.origin,image.blocks.size()*2}));
        const auto target=mount->node(13,"target"); CHECK(target);
        node=static_cast<Mount::MountedNode *>(target.value());
        // The declared native journal feature fixture is fixed before request
        // admission; no callback changes records or returned transport bytes.
        head(image,sequence,selected_features);
        for(u32 block=600;block<616;++block) { Owner::Pin pin; CHECK(mount->owner_.pin(block,pin)); CHECK(pin.release()); }
        ++fixtures;
    }
    void run_zero(bool noop)
    {
        clock_ready(); const auto before=mount->owner_.session_report().value();
        const auto epoch=mount->report_.epoch; const auto binding=mount->binding_; const Bytes view(mount->view_);
        const auto status=mount->typed_private_zero(*node,*workspace,*storage,0);
        if(!status) std::fprintf(stderr,"request zero code=%u message=%.*s\n",unsigned(status.code()),int(status.message().size()),status.message().data());
        CHECK(status); CHECK(storage->report_.published && storage->report_.renewed && storage->report_.receipt_consumed);
        CHECK(storage->report_.classified && storage->report_.inode==13 && storage->report_.generation==9);
        CHECK(storage->report_.classifier_reads==(noop ? 5U : 7U) && storage->report_.clock_samples==usize(!noop));
        CHECK(testing::ext4_clock::calls()==usize(!noop));
        CHECK(mount->report_.epoch==epoch+usize(!noop) && mount->report_.phase==Ext4MountPhase::readable);
        CHECK(!storage->busy_ && !storage->operation_ && !mount->mutation_context_ && !mount->mutation_lease_);
        CHECK(mount->owner_.mounted_teardown_preflight());
        const auto after=mount->owner_.session_report().value();
        if(noop) { CHECK(view.same(mount->view_) && reclaim_history::same_info(binding,mount->binding_));
            CHECK(before.writes==after.writes && before.flushes==after.flushes && storage->report_.suffix_reads==1); ++noops; }
        else { CHECK(storage->report_.epoch_committed && after.writes>before.writes && after.flushes>before.flushes); ++truncated; }
        const auto metadata=node->query_metadata(); CHECK(metadata && metadata.value().size==0);
        ++successful;
    }
    void run_refill()
    {
        clock_ready(); std::array<std::byte,66> input{};
        for(usize at=0;at<input.size();++at) input[at]=std::byte(1+(at+7)%251);
        const auto epoch=mount->report_.epoch;
        const auto status=mount->typed_private_refill(*node,*workspace,*storage,7,input);
        if(!status) std::fprintf(stderr,"request refill code=%u message=%.*s\n",unsigned(status.code()),int(status.message().size()),status.message().data());
        CHECK(status); CHECK(storage->report_.published && storage->report_.renewed && storage->report_.receipt_consumed);
        CHECK(storage->report_.classified && storage->report_.classifier_reads==5 && storage->report_.old_size==0);
        CHECK(testing::ext4_clock::calls()==1 && storage->report_.clock_samples==1 && mount->report_.epoch==epoch+1);
        CHECK(!storage->operation_ && !storage->busy_ && !mount->mutation_context_);
        const auto metadata=node->query_metadata(); CHECK(metadata && metadata.value().size==73);
        auto read=node->read(0,73); CHECK(read && read.value().size==73);
        for(usize at=0;at<7;++at) CHECK(read.value().data[at]==std::byte{});
        for(usize at=0;at<input.size();++at) CHECK(read.value().data[at+7]==input[at]);
        ++successful; ++allocations;
    }
    void clean_zero_refusal(StatusCode code)
    {
        clock_ready(); const auto before=mount->owner_.session_report().value();
        const auto epoch=mount->report_.epoch; const Bytes view(mount->view_); const auto binding=mount->binding_;
        CHECK(mount->typed_private_zero(*node,*workspace,*storage,0).code()==code);
        const auto after=mount->owner_.session_report().value();
        CHECK(storage->report_.phase==Phase::refused && !storage->report_.child_entered && !storage->report_.receipt_consumed);
        CHECK(!storage->report_.published && !storage->report_.renewed && testing::ext4_clock::calls()==0);
        CHECK(after.reads-before.reads==7 && after.reads==after.completed_reads && after.writes==before.writes && after.flushes==before.flushes);
        CHECK(mount->report_.epoch==epoch && mount->report_.phase==Ext4MountPhase::readable);
        CHECK(view.same(mount->view_) && reclaim_history::same_info(binding,mount->binding_));
        CHECK(!storage->operation_ && !storage->busy_ && !mount->mutation_context_ && mount->owner_.mounted_teardown_preflight());
        ++refused;
    }
};
void lifecycle(bool cross)
{
    { RequestFixture zero(cross,3); zero.run_zero(false); zero.run_zero(true); }
    { RequestFixture empty(cross,0); empty.run_zero(true); empty.run_zero(true); }
    { RequestFixture refill(cross,0,70,jbd2::feature_checksum_v3|jbd2::feature_revoke);
      refill.run_refill(); refill.run_zero(false); refill.run_zero(true); }
}
void ordinary_refusals(bool cross)
{
    RequestFixture sequence(cross,3,0xffffffffU); sequence.clean_zero_refusal(StatusCode::overflow);
    RequestFixture sequence_noop(cross,0,0xffffffffU); sequence_noop.run_zero(true);
    RequestFixture maximum_nonempty(cross,3);
    const auto epoch=maximum_nonempty.mount->report_.epoch;
    // Saved/restored idle scalar boundary. The actual request creates and owns
    // its original local context/Lease at this epoch; no serial is fabricated.
    maximum_nonempty.mount->report_.epoch=~u64{0}; maximum_nonempty.clean_zero_refusal(StatusCode::overflow);
    maximum_nonempty.mount->report_.epoch=epoch; ++maximum_epochs;
    maximum_nonempty.run_zero(false);
    RequestFixture maximum_empty(cross,0); const auto saved=maximum_empty.mount->report_.epoch;
    maximum_empty.mount->report_.epoch=~u64{0}; maximum_empty.run_zero(true);
    maximum_empty.mount->report_.epoch=saved; ++maximum_epochs;
    RequestFixture range(cross,0); clock_ready(); const auto native=range.mount->owner_.session_report().value();
    CHECK(range.mount->typed_private_zero(*range.node,*range.workspace,*range.storage,1).code()==StatusCode::unsupported);
    std::array<std::byte,2> data{};
    CHECK(range.mount->typed_private_refill(*range.node,*range.workspace,*range.storage,1023,data).code()==StatusCode::unsupported);
    CHECK(reclaim_history::same_session(native,range.mount->owner_.session_report().value()) && testing::ext4_clock::calls()==0); refused+=2;
    RequestFixture clock(cross,3); testing::ext4_clock::reset();
    const auto before=clock.mount->owner_.session_report().value();
    CHECK(clock.mount->typed_private_zero(*clock.node,*clock.workspace,*clock.storage,0).code()==StatusCode::io_error);
    const auto after=clock.mount->owner_.session_report().value();
    CHECK(clock.storage->report_.phase==Phase::refused && testing::ext4_clock::calls()==1);
    CHECK(after.writes==before.writes && after.flushes==before.flushes && clock.mount->report_.phase==Ext4MountPhase::readable); ++refused;
    RequestFixture child(cross,3,70,jbd2::feature_checksum_v3|0x20U); clock_ready();
    CHECK(!child.mount->typed_private_zero(*child.node,*child.workspace,*child.storage,0));
    CHECK(child.mount->report_.phase==Ext4MountPhase::failed && child.storage->report_.phase==Phase::failed);
    CHECK(child.storage->report_.child_entered && !child.storage->report_.receipt_consumed && !child.storage->report_.published && !child.storage->report_.suffix_reads);
    CHECK(child.mount->owner_.session_report().value().state==block::DeviceState::quarantined && !child.mount->mutation_context_); ++sticky;
}
struct LargeDevice final : driver::BlockDevice
{
    typed_owner_zero_large_fixture::Image &image;
    std::map<u32,Block> written;
    usize reads{},writes{},flushes{};
    explicit LargeDevice(typed_owner_zero_large_fixture::Image &value):image(value)
    {
        // The inherited planner fixture used an ordinary-file journal mode.
        // Declare the mounted profile's journal identity and empty head before
        // any Session opens; reads then return only persisted physical media.
        Block table{}; image.render(6,table);
        auto journal=std::span<u8,256>{table.data()+768,256};
        typed_owner_zero_large_fixture::put16(journal,0,0100600);
        typed_owner_zero_large_fixture::stamp_inode(journal,8,image.seed);
        written.emplace(6,table); written.emplace(1000,image.old_head);
    }
    driver::BlockGeometry geometry() const override { return {11+u64(typed_owner_zero_large_fixture::filesystem_blocks)*2,512,true}; }
    Status read_blocks(u64 sector,std::span<std::byte> output) override
    {
        CHECK(sector>=11 && (sector-11)%2==0 && output.size()==1024); ++reads;
        const auto number=u32((sector-11)/2); Block block{};
        if(auto found=written.find(number);found!=written.end()) block=found->second;
        else image.render(number,block);
        for(usize at=0;at<output.size();++at) output[at]=std::byte(block[at]);
        return {};
    }
    Status write_blocks(u64 sector,std::span<const std::byte> input) override
    {
        CHECK(sector>=11 && (sector-11)%2==0 && input.size()==1024); ++writes;
        auto &block=written[u32((sector-11)/2)];
        for(usize at=0;at<input.size();++at) block[at]=std::to_integer<u8>(input[at]);
        return {};
    }
    Status flush() override { ++flushes; return {}; }
};
void actual_u32_refusal()
{
    auto image=std::unique_ptr<typed_owner_zero_large_fixture::Image>(new typed_owner_zero_large_fixture::Image);
    image->initialize(false); LargeDevice device(*image); Owner::Registry registry;
    auto mount=std::unique_ptr<Mount>(new Mount(Ext4MountedPolicy::ordered_data_dirty_noatime));
    const auto key=registry.register_device(device,39999); CHECK(key);
    const auto mounted=mount->mount(registry,key.value(),{11,u64(typed_owner_zero_large_fixture::filesystem_blocks)*2});
    if(!mounted) std::fprintf(stderr,"request G32 mount code=%u message=%.*s reads=%zu writes=%zu flushes=%zu\n",
        unsigned(mounted.code()),int(mounted.message().size()),mounted.message().data(),device.reads,device.writes,device.flushes);
    CHECK(mounted);
    const auto target=mount->node(13,"target"); CHECK(target);
    auto &node=*static_cast<Mount::MountedNode *>(target.value());
    auto workspace=std::unique_ptr<Workspace>(new Workspace); auto storage=std::unique_ptr<Storage>(new Storage(*mount));
    const auto before=mount->owner_.session_report().value(); const auto serial=mount->owner_.typed_operation_serial_;
    const auto reads=device.reads; clock_ready();
    CHECK(mount->typed_private_request<u32>(node,*workspace,*storage,Kind::zero,0,0,{}).code()==StatusCode::overflow);
    CHECK(reclaim_history::same_session(before,mount->owner_.session_report().value()) && device.reads==reads);
    CHECK(mount->owner_.typed_operation_serial_==serial && !mount->owner_.typed_mounted_pending_);
    CHECK(!storage->operation_ && !storage->busy_ && !storage->report_.classifier_reads && testing::ext4_clock::calls()==0);
    CHECK(mount->report_.phase==Ext4MountPhase::readable && !mount->mutation_context_); ++counter_refusals;
    auto copied=before; const auto saved=copied.reads; copied.reads=~u32{0};
    CHECK(!Mount::typed_request_family_capacity<u32>(copied,32,typed_owner_zero_large_fixture::filesystem_blocks,Ext4MountedTypedOpening::zero_family));
    copied.reads=saved; CHECK(reclaim_history::same_session(before,copied)); ++scalar_checks;
}
}
int main()
{
    for(bool cross:{false,true}) { lifecycle(cross); ordinary_refusals(cross); }
    actual_u32_refusal();
    std::printf("MOUNTED_REQUEST_ZERO_REFILL successful=%zu zero_mutations=%zu refills=%zu noops=%zu refused=%zu sticky_child_failures=%zu maximum_epoch_cases=%zu actual_u32_refusals=%zu restored_scalar_checks=%zu checks=%zu\n",
        successful,truncated,allocations,noops,refused,sticky,maximum_epochs,counter_refusals,scalar_checks,checks);
    return 0;
}
