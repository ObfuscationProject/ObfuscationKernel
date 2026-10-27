// Hosted common opening with genuine native writer bytes and native transport.
#include "../mounted_typed_cold_startup/fixture.hpp"
#include "../ext4_realtime_model.hpp"
#include <ok/fs/ext4/mount.hpp>
#include <string>
using namespace typed_cold_startup_fixture;
namespace {
using Mount = Ext4MountedMetadata<2>;
using Work = Mount::TypedColdWorkspace;
using Publication = Mount::TypedColdPublication;
using Storage = Mount::TypedMutationStorage;
usize checks{}, recoveries{}, failures{}, quiet_refusals{}, mutations{}, noops{}, malformed{}, locator_failures{};
#define CHECK(x) do { ++checks; require(bool(x), #x); } while (false)
struct Fixture final {
    Device device;
    Owner::Registry registry{};
    std::unique_ptr<Ext4MountedStartupWorkspace> startup{new Ext4MountedStartupWorkspace};
    std::unique_ptr<Owner::TypedOwnerWorkspace> child{new Owner::TypedOwnerWorkspace};
    std::unique_ptr<Mount> mount{new Mount(Ext4MountedPolicy::ordered_data_dirty_noatime, nullptr, startup.get())};
    explicit Fixture(const std::vector<u8> &bytes) : device(bytes) {}
    template <typename Operation> Status run(Operation operation) {
        auto key=registry.register_device(device,60000+checks); CHECK(key);
        return mount->common_mount_scope(registry,key.value(),{0,image_bytes/512},*child,
            [&](const auto &original) {
                CHECK(device.reads==5 && !device.write_attempts && !device.flush_attempts);
                CHECK(original.common_ && mount->owner_.common_typed_selected());
                CHECK(original.common_->baseline().session_serial==original.serial);
                CHECK(!original.common_->baseline().reads && !original.common_->baseline().completed_reads);
                CHECK(original.common_->endpoint().reads==5 && original.common_->endpoint().completed_reads==5);
                CHECK(original.common_->whole_union()==Ext4MountedCommonOpeningBudget::whole_union());
                auto before=mount->owner_.session_report(); CHECK(before);
                CHECK(reclaim_history::same_session(before.value(),original.common_->endpoint()));
                Owner::Pin pin;
                CHECK(!mount->owner_.pin(1,pin)); CHECK(!mount->owner_.rearm()); CHECK(!mount->owner_.close());
                CHECK(!mount->retain_root()); quiet_refusals+=4;
                auto after=mount->owner_.session_report(); CHECK(after);
                CHECK(reclaim_history::same_session(before.value(),after.value()));
                CHECK(device.reads==5 && !device.write_attempts && !device.flush_attempts);
                std::unique_ptr<Work> work{new Work(*mount,original,*child)};
                std::unique_ptr<Publication> publication{new Publication(*work)};
                return operation(*work,*publication);
            });
    }
    void endpoint() {
        auto actual=mount->owner_.session_report(); CHECK(actual);
        CHECK(actual.value().session_serial==mount->identity_.session_serial);
        CHECK(actual.value().reads==device.reads && actual.value().completed_reads==device.reads);
        CHECK(actual.value().writes==device.write_attempts && actual.value().completed_writes==device.write_attempts);
        CHECK(actual.value().flushes==device.flush_attempts && actual.value().completed_flushes==device.flush_attempts);
        CHECK(!actual.value().pending_writes && !actual.value().in_flight);
    }
};
std::array<usize,3> positive(const NativePreparation &prepared,u8 mask,bool inspect_classifier) {
    Fixture f(prepared.cut(mask));
    const auto opened=f.run([&](Work &work,Publication &publication) {
        Status status;
        if (inspect_classifier) {
            CHECK(f.mount->owner_.adopt_common_typed_cursor(*f.mount->owner_.common_opening_->selection_,work.attachment_));
            CHECK(f.device.reads==5 && work.cursor_.control_.classifier_reads==5);
            const auto before=work.session_.report(); CHECK(before);
            CHECK(f.mount->typed_classify_common_sources(work));
            const auto after=work.session_.report(); CHECK(after);
            CHECK(reclaim_history::same_session(before.value(),after.value()));
            CHECK(f.device.reads==5 && !f.device.write_attempts && !f.device.flush_attempts);
            CHECK(f.mount->typed_run_cold_child(work)); CHECK(f.mount->typed_run_cold_suffix(work));
            status=f.mount->typed_publish_cold_root(publication);
        } else status=f.mount->typed_run_common_operation(work,publication);
        require(status,"genuine common typed recovery"); ++checks;
        CHECK(work.cursor_.common_adopted_ && work.cursor_.control_.classifier_reads==5);
        CHECK(reclaim_history::same_session(work.cursor_.initial_,work.original_.common_->baseline()));
        CHECK(work.semantic_mask_==510 && work.root_checked_ && work.root_.number==2);
        CHECK(publication.completed_ && publication.renewed_ && work.attachment_.renewed_);
        CHECK(work.receipt_.minted_ && work.receipt_.consumed_);
        CHECK(f.mount->report().phase==Ext4MountPhase::readable && f.mount->report().epoch==1);
        f.endpoint(); return status;
    });
    require(opened,"original common scope terminal"); ++checks;
    CHECK(!f.mount->owner_.common_opening_ && !f.mount->typed_cold_opening_);
    CHECK(f.mount->owner_.startup_report_.phase==Ext4StartupPhase::complete);
    CHECK(!f.mount->owner_.startup_report_.root_unpublished);
    CHECK(f.mount->owner_.startup_report_.final_sequence==prepared.final_sequence);
    CHECK(f.child->owner_==&f.mount->owner_ && f.child->phase_==Owner::TypedOwnerPhase::ready);
    CHECK(f.device.bytes==prepared.completed()); f.endpoint();
    auto root=f.mount->retain_root(); CHECK(root); CHECK(root.value().node_for_mount()); CHECK(root.value().release());
    CHECK(f.mount->owner_.mounted_teardown_preflight());
    const auto trace=std::array<usize,3>{f.device.reads,f.device.write_attempts,f.device.flush_attempts};
    CHECK(f.mount->close()); CHECK(f.mount->report().phase==Ext4MountPhase::closed);
    ++recoveries; return trace;
}
void rejected(const std::vector<u8> &bytes,usize read,usize write,usize flush,bool io) {
    Fixture f(bytes); f.device.fail_read=read; f.device.fail_write=write; f.device.fail_flush=flush;
    bool invoked{};
    const auto result=f.run([&](Work &work,Publication &publication) {
        invoked=true; const auto status=f.mount->typed_run_common_operation(work,publication);
        CHECK(!status && !publication.completed_); return status;
    });
    CHECK(!result); if (io) CHECK(result.code()==StatusCode::io_error);
    CHECK(f.mount->owner_.startup_report_.phase==Ext4StartupPhase::failed &&
        f.mount->owner_.startup_report_.status.code()==result.code());
    CHECK(!f.mount->owner_.common_opening_ && !f.mount->typed_cold_opening_);
    CHECK(f.mount->report().phase==Ext4MountPhase::failed && !f.mount->report().epoch && !f.mount->report().published);
    for (const auto &node:f.mount->nodes_) CHECK(!node.inode_);
    CHECK(!f.mount->retain_root());
    if (read && read<=5) { CHECK(!invoked && f.device.reads==read && !f.device.write_attempts && !f.device.flush_attempts); ++locator_failures; }
    ++failures;
}
void controls(const NativePreparation &prepared,const std::array<usize,3> &trace) {
    const auto bytes=prepared.cut(0);
    for (usize read=1;read<=6;++read) rejected(bytes,read,0,0,true);
    rejected(bytes,trace[0],0,0,true);
    for (usize write=1;write<=trace[1];++write) rejected(bytes,0,write,0,true);
    for (usize flush=1;flush<=trace[2];++flush) rejected(bytes,0,0,flush,true);
    auto corrupt=bytes; corrupt[usize(prepared.binding.layout.journal_first_block+1)*1024+136]^=1;
    // Recognized typed media-envelope corruption is prepared before opening.
    // The real common locator cannot fall through to a legacy writer.
    Fixture f(corrupt); bool invoked{};
    const auto result=f.run([&](Work &work,Publication &publication) {
        invoked=true; const auto status=f.mount->typed_run_common_operation(work,publication);
        CHECK(!status && !publication.completed_); return status;
    });
    CHECK(!result && invoked && f.device.reads==5 && !f.device.write_attempts && !f.device.flush_attempts);
    CHECK(!f.mount->report().epoch && !f.mount->nodes_[0].inode_ && !f.mount->retain_root()); ++malformed;
}
void sequence_refusal(const NativePreparation &prepared,u32 sequence) {
    auto bytes=prepared.cut(0);
    const usize first=usize(prepared.binding.layout.journal_first_block)*1024;
    auto block=[&](usize relative) { return std::span<u8>{bytes}.subspan(first+relative*1024,1024); };
    auto big=[](std::span<u8> out,usize at,u32 value) { for (usize n=0;n<4;++n) out[at+n]=u8(value>>(24-n*8)); };
    auto little=[](std::span<u8> out,usize at,u32 value) { for (usize n=0;n<4;++n) out[at+n]=u8(value>>(n*8)); };
    const u32 seed=jbd2::detail::crc32c(~u32{0},prepared.binding.uuid);
    auto head=block(0),descriptor=block(1),commit=block(6);
    big(head,24,sequence); big(head,252,0); big(head,252,jbd2::detail::crc32c(~u32{0},head));
    big(descriptor,8,sequence); little(descriptor,160,sequence);
    for (usize home=0;home<4;++home) {
        const usize at=home?44+(home-1)*16:12;
        big(descriptor,at+12,jbd2::detail::crc32c(jbd2::detail::crc32c(seed,descriptor.subspan(8,4)),block(2+home)));
    }
    big(descriptor,1020,0); big(descriptor,1020,jbd2::detail::crc32c(seed,descriptor));
    big(commit,8,sequence); big(commit,16,0); big(commit,16,jbd2::detail::crc32c(seed,commit));
    // Pure pre-opening media capacity controls; no native history is changed.
    const auto codec=jbd2::Codec::open(head,prepared.binding.layout.journal_blocks,prepared.binding.uuid,
        jbd2::DescriptorUuidPolicy::linux_internal_zero); CHECK(codec);
    usize home{};
    CHECK(codec.value().descriptor(descriptor,sequence,[&](const jbd2::Tag &tag) {
        return codec.value().payload(block(2+home++),sequence,tag);
    })); CHECK(home==4); CHECK(codec.value().commit(commit,sequence));
    Fixture f(bytes);
    const auto refused=f.run([&](Work &work,Publication &publication) {
        const auto status=f.mount->typed_run_common_operation(work,publication);
        CHECK(!status && status.code()==StatusCode::overflow && !publication.attempted_);
        CHECK(f.device.reads==5 && !f.device.write_attempts && !f.device.flush_attempts);
        CHECK(!work.owner_workspace_.attempted_); return status;
    });
    CHECK(!refused && !f.mount->report().epoch && !f.mount->report().published); ++quiet_refusals;
}
void clock_ready() {
    testing::ext4_clock::reset(); testing::ext4_clock::set({{1700000333,123456789},
        {1700000333000000000ULL,1},1,time::RealtimeOrigin::administrator_software});
}
void composition(const NativePreparation &prepared) {
    Fixture f(prepared.cut(0));
    require(f.run([&](Work &work,Publication &publication) { return f.mount->typed_run_common_operation(work,publication); }),
        "common opening followed by mounted requests"); ++checks;
    std::unique_ptr<Storage> mutation{new Storage(*f.mount)};
    auto retained=f.mount->retain_root(); CHECK(retained); auto root=retained.value().node_for_mount(); CHECK(root);
    auto lookup=root.value()->lookup_child_retained(prepared.allocated?"empty":"zero"); CHECK(lookup);
    auto &node=*static_cast<Mount::MountedNode *>(lookup.value());
    for (usize cycle=0;cycle<2;++cycle) {
        clock_ready(); CHECK(f.mount->typed_private_zero(node,*f.child,*mutation,0));
        CHECK(mutation->report_.receipt_consumed && mutation->report_.renewed && mutation->report_.published); f.endpoint();
        clock_ready(); const auto before=f.mount->owner_.session_report(); CHECK(before);
        CHECK(f.mount->typed_private_zero(node,*f.child,*mutation,0));
        auto after=f.mount->owner_.session_report(); CHECK(after);
        CHECK(after.value().writes==before.value().writes && after.value().flushes==before.value().flushes);
        CHECK(!testing::ext4_clock::calls()); ++noops;
        std::array<std::byte,66> text{}; for (usize at=0;at<text.size();++at) text[at]=std::byte('A'+at%26);
        clock_ready(); CHECK(f.mount->typed_private_refill(node,*f.child,*mutation,7,text));
        CHECK(mutation->report_.receipt_consumed && mutation->report_.renewed && mutation->report_.published); f.endpoint();
        auto read=node.read(0,73); CHECK(read && read.value().size==73);
        for (usize at=0;at<7;++at) CHECK(read.value().data[at]==std::byte{});
        for (usize at=0;at<text.size();++at) CHECK(read.value().data[at+7]==text[at]);
        ++mutations;
    }
    CHECK(node.release()); CHECK(retained.value().release()); CHECK(f.mount->close());
}
}
int main(int argc,char **argv) {
    CHECK(argc==3); const auto input=load(argv[1]);
    for (bool allocated:{false,true}) {
        const auto prepared=prepare(input,allocated); std::array<usize,3> trace{};
        const std::string output=std::string(argv[2])+(allocated?"/allocated-":"/zero-");
        save((output+"active.img").c_str(),prepared.cut(0));
        save((output+"completed.img").c_str(),prepared.completed());
        for (u8 mask=0;mask<16;++mask) { const auto actual=positive(prepared,mask,!mask); if (!mask) trace=actual; }
        controls(prepared,trace); sequence_refusal(prepared,1); sequence_refusal(prepared,0xfffffffeU); composition(prepared);
    }
    std::printf("{\"passed\":true,\"recoveries\":%zu,\"failures\":%zu,\"quiet_refusals\":%zu,\"refills\":%zu,\"noops\":%zu,\"malformed_no_fallback\":%zu,\"locator_failures\":%zu,\"checks\":%zu}\n",
        recoveries,failures,quiet_refusals,mutations,noops,malformed,locator_failures,checks);
}
