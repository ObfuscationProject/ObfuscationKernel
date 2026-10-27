// Hosted vertical slice over actual generated media and original native history.
#include "fixture.hpp"
#include <ok/fs/ext4/mount.hpp>
#include <set>
using namespace typed_cold_startup_fixture;
namespace {
using Mount = Ext4MountedMetadata<2>;
using Work = Mount::TypedColdWorkspace;
using Publication = Mount::TypedColdPublication;
usize checks{}, positives{}, failures{}, publications{}, root_reads{}, crash_cuts{}, mixed_home{}, quiet_refusals{}, io_failures{}, malformed{}, legacy_refusals{}, scalar_comparisons{};
#define CHECK(x) do { ++checks; require(bool(x), #x); } while (false)
struct Fixture final {
    Device device;
    Owner::Registry registry{};
    std::unique_ptr<Ext4MountedStartupWorkspace> startup{new Ext4MountedStartupWorkspace};
    std::unique_ptr<Owner::TypedOwnerWorkspace> owner_workspace{new Owner::TypedOwnerWorkspace};
    std::unique_ptr<Mount> mount{new Mount(Ext4MountedPolicy::ordered_data_dirty_noatime, nullptr, startup.get())};
    explicit Fixture(const std::vector<u8> &bytes) : device(bytes) {}
    template <typename Operation> Status run(Operation operation) {
        auto key = registry.register_device(device, 700 + positives + failures); CHECK(key);
        return mount->typed_cold_mount_scope(registry, key.value(), {0, image_bytes / 512}, *owner_workspace,
            [&](const auto &original) {
                std::unique_ptr<Work> work{new Work(*mount, original, *owner_workspace)};
                std::unique_ptr<Publication> publication{new Publication(*work)};
                return operation(*work, *publication);
            });
    }
};
struct Trace { std::array<usize,3> physical{}; std::set<usize> selected_reads; };
Trace positive(const NativePreparation &prepared, u8 mask) {
    Fixture f(prepared.cut(mask));
    Trace trace; std::array<usize,10> stage_first{}, stage_last{};
    for (usize at=1; at<=6; ++at) trace.selected_reads.insert(at);
    CHECK(f.run([&](Work &work, Publication &publication) {
        f.device.observe_read=[&](usize physical) {
            const usize stage=static_cast<usize>(work.executing_);
            if (stage) { CHECK(stage<stage_first.size()); if (!stage_first[stage]) stage_first[stage]=physical; stage_last[stage]=physical; }
        };
        CHECK(f.mount->typed_admit_cold_cursor(work));
        CHECK(f.device.reads == 0 && f.device.write_attempts == 0 && f.device.flush_attempts == 0);
        CHECK(work.cursor_.capacity_.origin == Ext4MountedTypedOrigin::startup);
        const auto quiet = [&] {
            Owner::Pin pin;
            CHECK(!f.mount->typed_admit_cold_cursor(work));
            CHECK(!f.mount->typed_publish_cold_root(publication));
            CHECK(!f.mount->owner_.typed_renew_mounted_workspace(work.attachment_, work.receipt_));
            CHECK(!f.mount->owner_.pin(1, pin)); CHECK(!f.mount->owner_.rearm()); CHECK(!f.mount->owner_.close());
            quiet_refusals += 6;
            CHECK(f.device.reads == 0 && f.device.write_attempts == 0 && f.device.flush_attempts == 0);
        };
        quiet();
        CHECK(f.mount->typed_classify_cold(work));
        CHECK(f.device.reads == 5 && work.cursor_.control_.classifier_reads == 5);
        CHECK(f.mount->typed_run_cold_child(work));
        const auto child = work.attachment_.child_terminal_;
        CHECK(work.owner_workspace_.prefix_reservation_==8 && work.owner_workspace_.prefix_reads_==0);
        trace.selected_reads.insert(child.reads); trace.selected_reads.insert(child.reads+1);
        CHECK(child.session_serial == work.original_.serial);
        CHECK(f.mount->typed_run_cold_suffix(work));
        CHECK(f.mount->report().phase == Ext4MountPhase::opening);
        for (const auto &node : f.mount->nodes_) CHECK(node.inode_ == 0);
        CHECK(work.root_checked_ && work.root_.number == 2);
        CHECK(work.semantic_mask_ == 510 && work.cursor_.control_.stage_index == 8);
        CHECK(work.receipt_.minted_ && work.receipt_.consumed_);
        const auto before = work.session_.report(); CHECK(before);
        const auto physical = std::array<usize, 3>{f.device.reads, f.device.write_attempts, f.device.flush_attempts};
        CHECK(f.mount->typed_publish_cold_root(publication));
        const auto after = work.session_.report(); CHECK(after);
        CHECK(reclaim_history::same_session(before.value(), after.value()));
        CHECK((physical == std::array<usize,3>{f.device.reads,f.device.write_attempts,f.device.flush_attempts}));
        CHECK(publication.completed_ && publication.renewed_ && work.attachment_.renewed_);
        CHECK(!f.mount->typed_publish_cold_root(publication));
        CHECK(!f.mount->owner_.typed_renew_mounted_workspace(work.attachment_, work.receipt_));
        quiet_refusals += 2;
        CHECK((physical == std::array<usize,3>{f.device.reads,f.device.write_attempts,f.device.flush_attempts}));
        const auto detached = work.root_;
        auto restored = detached;
        for (u32 changed=0; changed<7; ++changed) {
            switch (changed) {
            case 0: ++restored.number; break; case 1: ++restored.uid; break; case 2: ++restored.gid; break;
            case 3: ++restored.size; break; case 4: ++restored.generation; break;
            case 5: ++restored.mode; break; default: ++restored.links; break;
            }
            CHECK(!Mount::typed_cold_same_root(detached, restored));
            restored = detached; CHECK(Mount::typed_cold_same_root(detached, restored)); scalar_comparisons += 2;
        }
        CHECK(f.mount->nodes_[0].inode_ == 2 && f.mount->nodes_[0].lifetime_key_.generation == work.root_.generation);
        CHECK(f.mount->report().phase == Ext4MountPhase::readable && f.mount->report().epoch == 1);
        CHECK(f.device.bytes == prepared.completed());
        ++publications; root_reads += usize(work.cursor_.control_.suffix_reads);
        f.device.observe_read={};
        return Status{};
    }));
    auto root = f.mount->retain_root(); CHECK(root); CHECK(root.value().node_for_mount());
    CHECK(root.value().release());
    // A permanently claimed genuine renewed workspace cannot migrate to a
    // different new Mount/Owner, even though its child attempted_ bit is clear.
    Fixture foreign(f.device.bytes);
    auto foreign_key=foreign.registry.register_device(foreign.device,1001); CHECK(foreign_key);
    bool invoked{};
    const auto endpoint=f.mount->owner_.session_report(); CHECK(endpoint);
    CHECK(!foreign.mount->typed_cold_mount_scope(foreign.registry,foreign_key.value(),{0,image_bytes/512},
        *f.owner_workspace,[&](const auto &) { invoked=true; return Status{}; }));
    CHECK(!invoked && !foreign.device.reads && !foreign.device.write_attempts && !foreign.device.flush_attempts);
    CHECK(foreign.mount->report().phase==Ext4MountPhase::unused);
    const auto retained=f.mount->owner_.session_report(); CHECK(retained);
    CHECK(reclaim_history::same_session(endpoint.value(),retained.value()));
    CHECK(f.owner_workspace->owner_==&f.mount->owner_ && f.owner_workspace->phase_==Owner::TypedOwnerPhase::ready);
    ++quiet_refusals;
    ++positives;
    if (prepared.writer_crash_reachable(mask)) ++crash_cuts; else ++mixed_home;
    trace.physical={f.device.reads,f.device.write_attempts,f.device.flush_attempts};
    trace.selected_reads.insert(f.device.reads);
    for (usize at=1;at<=8;++at) {
        CHECK(stage_first[at] && stage_last[at]>=stage_first[at]);
        trace.selected_reads.insert(stage_first[at]); trace.selected_reads.insert(stage_last[at]);
    }
    return trace;
}
void rejected(const std::vector<u8> &bytes, usize fail_read, usize fail_write, usize fail_flush) {
    Fixture f(bytes); f.device.fail_read=fail_read; f.device.fail_write=fail_write; f.device.fail_flush=fail_flush;
    Status observed;
    const auto result = f.run([&](Work &work, Publication &publication) {
        observed = f.mount->typed_run_cold_operation(work, publication);
        if (observed) std::fprintf(stderr,"unexpected success fail_read=%zu fail_write=%zu fail_flush=%zu actual=%zu/%zu/%zu suffix=%llu\n",fail_read,fail_write,fail_flush,f.device.reads,f.device.write_attempts,f.device.flush_attempts,static_cast<unsigned long long>(work.cursor_.control_.suffix_reads));
        CHECK(!observed && !publication.completed_);
        CHECK(!f.mount->report().epoch && f.mount->report().phase == Ext4MountPhase::failed);
        CHECK(!f.mount->report().published && f.mount->nodes_[0].inode_ == 0);
        for (const auto &node : f.mount->nodes_) CHECK(node.inode_ == 0);
        CHECK(!f.mount->retain_root());
        return observed;
    });
    CHECK(!result && result.code()==observed.code());
    if (fail_read || fail_write || fail_flush) { CHECK(result.code()==StatusCode::io_error); ++io_failures; }
    ++failures;
}
void sequence_refusal(const NativePreparation &prepared, u32 sequence) {
    auto bytes=prepared.cut(0);
    const usize first=usize(prepared.binding.layout.journal_first_block)*1024;
    auto block=[&](usize relative) { return std::span<u8>{bytes}.subspan(first+relative*1024,1024); };
    auto big=[](std::span<u8> out,usize at,u32 value) { for (usize n=0;n<4;++n) out[at+n]=u8(value>>(24-n*8)); };
    auto little=[](std::span<u8> out,usize at,u32 value) { for (usize n=0;n<4;++n) out[at+n]=u8(value>>(n*8)); };
    const u32 seed=jbd2::detail::crc32c(~u32{0},prepared.binding.uuid);
    auto head=block(0), descriptor=block(1), commit=block(6);
    big(head,24,sequence); big(head,252,0); big(head,252,jbd2::detail::crc32c(~u32{0},head));
    big(descriptor,8,sequence); little(descriptor,160,sequence);
    for (usize home=0;home<4;++home) {
        const usize at=home ? 44+(home-1)*16 : 12;
        big(descriptor,at+12,jbd2::detail::crc32c(jbd2::detail::crc32c(seed,descriptor.subspan(8,4)),block(2+home)));
    }
    big(descriptor,1020,0); big(descriptor,1020,jbd2::detail::crc32c(seed,descriptor));
    big(commit,8,sequence); big(commit,16,0); big(commit,16,jbd2::detail::crc32c(seed,commit));
    // These are pre-Session media-envelope capacity controls. They are not a
    // claim that a native writer produced an exhausted sequence history.
    const auto codec=jbd2::Codec::open(head,prepared.binding.layout.journal_blocks,prepared.binding.uuid,
        jbd2::DescriptorUuidPolicy::linux_internal_zero); CHECK(codec);
    usize home{};
    CHECK(codec.value().descriptor(descriptor,sequence,[&](const jbd2::Tag &tag) {
        return codec.value().payload(block(2+home++),sequence,tag);
    })); CHECK(home==4); CHECK(codec.value().commit(commit,sequence));
    Fixture f(bytes);
    CHECK(!f.run([&](Work &work,Publication &publication) {
        CHECK(f.mount->typed_admit_cold_cursor(work));
        const auto refused=f.mount->typed_classify_cold(work);
        CHECK(!refused && refused.code()==StatusCode::overflow);
        CHECK(f.device.reads==5 && !f.device.write_attempts && !f.device.flush_attempts);
        CHECK(!work.owner_workspace_.attempted_ && !publication.attempted_);
        ++quiet_refusals; return refused;
    }));
    CHECK(!f.mount->report().epoch && !f.mount->report().published && !f.mount->nodes_[0].inode_);
}
void ordinary_route(const std::vector<u8> &bytes) {
    Device inspected(bytes); Ext4RecoveryBinding inspection;
    CHECK(inspection.inspect(inspected,{0,image_bytes/512})); CHECK(inspection.info());
    auto ordinary=bytes;
    auto head=std::span<u8>{ordinary}.subspan(usize(inspection.info()->layout.journal_first_block)*1024,1024);
    auto big=[](std::span<u8> out,usize at,u32 value) { for (usize n=0;n<4;++n) out[at+n]=u8(value>>(24-n*8)); };
    // The unchanged general ordinary completion supports CRC32C-v3 without
    // revoke. Select that empty-media profile before opening any native Session.
    big(head,40,jbd2::feature_checksum_v3); big(head,252,0);
    big(head,252,jbd2::detail::crc32c(~u32{0},head));
    Fixture f(ordinary);
    auto key=f.registry.register_device(f.device,940); CHECK(key);
    require(f.mount->mount(f.registry,key.value(),{0,image_bytes/512}),"existing ordinary startup"); ++checks;
    CHECK(f.mount->report().epoch==1 && f.mount->report().phase==Ext4MountPhase::readable);
    auto root=f.mount->retain_root(); CHECK(root); CHECK(root.value().release());
}
void controls(const NativePreparation &prepared, const Trace &successful_trace) {
    sequence_refusal(prepared,1); sequence_refusal(prepared,0xfffffffeU);
    const auto cut=prepared.cut(0);
    // Bound READ fault coverage to every locator READ, actual child entry and
    // terminal, and each actual semantic stage entry/terminal including root.
    // Generic leaf READ faults have separate accepted native cursor/child proof.
    for (usize at : successful_trace.selected_reads) rejected(cut,at,0,0);
    for (usize at=1; at<=successful_trace.physical[1]; ++at) rejected(cut,0,at,0);
    for (usize at=1; at<=successful_trace.physical[2]; ++at) rejected(cut,0,0,at);
    auto corrupt=cut;
    const usize descriptor=usize(prepared.binding.layout.journal_first_block+1)*1024;
    corrupt[descriptor+136]^=1; // Recognized magic, malformed physical CRC/version.
    rejected(corrupt,0,0,0); ++malformed;
    // Existing native startup must explicitly refuse the recognized typed family.
    Fixture legacy(cut);
    auto key=legacy.registry.register_device(legacy.device,930); CHECK(key);
    const auto refusal=legacy.mount->mount(legacy.registry,key.value(),{0,image_bytes/512});
    CHECK(!refusal && refusal.code()==StatusCode::unsupported);
    CHECK(legacy.device.reads==5 && !legacy.device.write_attempts && !legacy.device.flush_attempts);
    CHECK(!legacy.mount->report().epoch && !legacy.mount->report().published && !legacy.mount->nodes_[0].inode_);
    ++legacy_refusals;
    // Real untouched counters, deliberately smaller declared finite width: the
    // union cannot fit and is refused before any physical READ or writer.
    Fixture narrow(cut);
    CHECK(!narrow.run([&](Work &work, Publication &) {
        CHECK(narrow.mount->typed_cold_original_exact(work));
        CHECK(narrow.mount->owner_.typed_admit_mounted_cursor(work.attachment_));
        const auto refused=work.cursor_.reserve<u16>(work.context_,4,32768,Ext4MountedTypedOrigin::startup,
            Ext4MountedTypedOpening::startup_recovery_family);
        CHECK(!refused && refused.code()==StatusCode::overflow);
        CHECK(!narrow.device.reads && !narrow.device.write_attempts && !narrow.device.flush_attempts);
        return refused;
    }));
    ++quiet_refusals;
}

}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    const auto input = load(argv[1]);
    ordinary_route(input);
    for (bool allocated : {false, true}) {
        const auto prepared = prepare(input, allocated);
        Trace successful_trace{};
        for (u8 mask=0; mask<16; ++mask) { const auto trace=positive(prepared,mask); if (!mask) successful_trace=trace; }
        controls(prepared,successful_trace);
    }
    std::printf("{\"passed\":true,\"positives\":%zu,\"failures\":%zu,\"publications\":%zu,\"writer_crash_cuts\":%zu,\"mixed_home_inputs\":%zu,\"suffix_reads\":%zu,\"quiet_refusals\":%zu,\"io_failures\":%zu,\"malformed\":%zu,\"legacy_refusals\":%zu,\"detached_restored_scalar_comparisons\":%zu,\"checks\":%zu}\n",
        positives, failures, publications, crash_cuts, mixed_home, root_reads, quiet_refusals, io_failures, malformed, legacy_refusals, scalar_comparisons, checks);
}
