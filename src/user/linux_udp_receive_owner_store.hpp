#pragma once
#include "linux_udp_receive_authority.hpp"
namespace ok::user::native
{
// Private concrete producer shared by configured runtime and target models.
// A copied request or raw pool object cannot enter its capture transfer path.
template <abi::linux_user::Profile Selected, memory::ObjectPageProvider Provider>
class LinuxUdpReceiveOwnerStore final : private NonCopyable
{
  public:
    using Origins=SyscallOriginTable<Selected>;
    using Owner=LinuxUdpReceiveOwner<Selected>;
    using Coordinator=LinuxUdpReceiveCoordinator<Selected>;
    using Access=LinuxUdpImmediateAccess<Selected>;
    using Admission=LinuxUdpReceiveAdmission<Selected>;
    static constexpr usize capacity=memory::KernelEntryStackPool::capacity;
    using Layout=memory::ObjectPool<Owner,Provider,1>;
    static constexpr usize pages=(capacity+Layout::slots_per_page-1)/Layout::slots_per_page;
    using Objects=memory::ObjectPool<Owner,Provider,pages>;
    struct Member
    {
        sched::ProcessId pid{};
        sched::ThreadId tid{};
        memory::ObjectHandle object{};
        u64 reservation{};
    };
    struct Staged
    {
        memory::ObjectHandle object{};
        Status status{};
        bool contained{};
        [[nodiscard]] bool ok() const { return status.ok(); }
    };
    constexpr LinuxUdpReceiveOwnerStore(Origins &issuer, Provider &provider)
        : issuer_(issuer),objects_(provider,capacity) {}
    [[nodiscard]] constexpr Objects &objects() { return objects_; }
    [[nodiscard]] constexpr auto &members() { return members_; }
    [[nodiscard]] bool contained() const { return contained_; }
    [[nodiscard]] Status failure() const { return failure_; }
    [[nodiscard]] Status validate(Kernel &kernel, sched::ProcessControlBlock &process,
        sched::ThreadControlBlock &thread, Access &access, const Admission &admission,
        const linux_udp::OriginalUdpDescription &original)
    {
        if(contained_) return failure_;
        return Coordinator::validate_store(issuer_,kernel,process,thread,access,admission,original);
    }
    [[nodiscard]] Member *member(memory::ObjectHandle object)
    {
        for(auto &entry:members_) if(entry.pid && entry.object==object) return &entry;
        return nullptr;
    }
    [[nodiscard]] Staged stage(Kernel &kernel, sched::ProcessControlBlock &process,
        sched::ThreadControlBlock &thread, Access &access, linux_udp::OriginalUdpDescription &original,
        Admission &admission)
    {
        if(contained_) return {{},failure_,true};
        const auto current=[&] { return validate(kernel,process,thread,access,admission,original); };
        if(const auto valid=current(); !valid) return {{},valid,false};
        usize slot=capacity;
        for(usize index=0;index<capacity;++index)
        {
            const auto &entry=members_[index];
            if(entry.pid==process.pid() && entry.tid==thread.tid)
                return {{},Status::fault("duplicate Linux UDP receive ownership"),false};
            if(!entry.pid && slot==capacity) slot=index;
        }
        if(slot==capacity) return {{},Status::no_memory("Linux UDP receive membership pressure"),false};
        if(sequences_[slot]==~u64{0}) return {{},Status::overflow("Linux UDP reservation generation exhausted"),false};
        const Reservation reservation{this,slot,++sequences_[slot],process.pid(),thread.tid};
        members_[slot]={reservation.pid,reservation.tid,{},reservation.generation};
        const auto created=objects_.create(reservation.pid,reservation.tid);
        if(!created)
        {
            erase(reservation,{});
            if(objects_.accounting().quarantined_pages) contain(created.status());
            return {{},created.status(),contained_};
        }
        const auto object=created.value();
        // Provider callbacks can clear and reuse even the same PID/TID slot.
        // Exact store/slot/generation and issuer must still match afterward.
        if(!reserved(reservation,{}))
            return refuse(reservation,object,Status::fault("Linux UDP receive allocation lost exact reservation"));
        if(const auto valid=current(); !valid) return refuse(reservation,object,valid);
        members_[slot].object=object;
        const auto resolved=objects_.resolve(object);
        if(!resolved) return refuse(reservation,object,resolved.status());
        auto &owner=*resolved.value();
        if(owner.pid!=reservation.pid || owner.tid!=reservation.tid || !reserved(reservation,object))
            return refuse(reservation,object,Status::fault("Linux UDP staged object lost exact membership"));
        auto prepared=Coordinator::prepare(access,admission,original,object,owner);
        if(!prepared) return refuse(reservation,object,prepared.status());
        // No provider, scheduler/user boundary or callback between these calls.
        const auto transferred=Coordinator::commit(prepared.value(),access,original,admission,owner);
        if(!transferred) return refuse(reservation,object,transferred);
        return {object,{},false};
    }
  private:
    struct Reservation
    {
        const LinuxUdpReceiveOwnerStore *store{};
        usize slot{};
        u64 generation{};
        sched::ProcessId pid{};
        sched::ThreadId tid{};
    };
    [[nodiscard]] bool reserved(const Reservation &id, memory::ObjectHandle object) const
    {
        if(id.store!=this || id.slot>=capacity || !id.generation || sequences_[id.slot]!=id.generation) return false;
        const auto &member=members_[id.slot];
        return member.reservation==id.generation && member.pid==id.pid && member.tid==id.tid && member.object==object;
    }
    void erase(const Reservation &id, memory::ObjectHandle object)
    { if(reserved(id,object)) members_[id.slot]={}; }
    void contain(Status first)
    { if(!contained_) { failure_=first; contained_=true; } }
    [[nodiscard]] Staged refuse(const Reservation &id, memory::ObjectHandle object, Status first)
    {
        erase(id,{}); erase(id,object); // Preserve every replacement reservation.
        const auto destroyed=objects_.destroy(object);
        const auto trimmed=objects_.trim_empty_pages();
        if(!destroyed || !trimmed) contain(first);
        return {{},first,contained_};
    }
    Origins &issuer_;
    Objects objects_;
    std::array<Member,capacity> members_{};
    std::array<u64,capacity> sequences_{};
    Status failure_{};
    bool contained_{};
};
} // namespace ok::user::native
