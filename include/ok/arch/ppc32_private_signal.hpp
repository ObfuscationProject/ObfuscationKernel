#pragma once

#include "ok/user/native_signal_config.hpp"
#include "ok/arch/execution.hpp"

namespace ok::arch::execution
{
// The retained proof survives deactivation, but not another activation,
// release, quarantine or reuse. It never exposes pool record internals.
struct Ppc32PrivateViewAccess final
{
    struct RecordSnapshot
    {
        const void *record{};
        uptr lower{},top{};
        u64 pid{},tid{},generation{},epoch{};
        bool inactive{};
    };
    template<class Pool>
    [[nodiscard]] static RecordSnapshot inspect_record(const Pool &pool,u32 slot) noexcept
    {
#if (defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__powerpc__) || defined(__PPC__)) && __SIZEOF_POINTER__ == 4 && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) || defined(OK_PPC32_ADMISSION_MODEL)
        if(!pool.initialized_ || slot>=Pool::capacity) return {};
        const auto &r=pool.records_[slot];
        if(r.state!=KernelEntryStackView::State::reserved && r.state!=KernelEntryStackView::State::active) return {};
        return {&r,r.lower,r.top,r.pid,r.tid,r.generation,r.epoch,r.state==KernelEntryStackView::State::reserved};
#else
        (void)pool;(void)slot;return {};
#endif
    }
    [[nodiscard]] static bool matches_record(const KernelEntryStackView &view,const RecordSnapshot &record) noexcept
    {
#if (defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__powerpc__) || defined(__PPC__)) && __SIZEOF_POINTER__ == 4 && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) || defined(OK_PPC32_ADMISSION_MODEL)
        return view.valid() && !record.inactive && view.record_==record.record &&
            view.generation_==record.generation && view.epoch_==record.epoch;
#else
        (void)view;(void)record;return false;
#endif
    }

    [[nodiscard]] static bool retained(const KernelEntryStackView &v) noexcept
    {
#if (defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__powerpc__) || defined(__PPC__)) && __SIZEOF_POINTER__ == 4 && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) || defined(OK_PPC32_ADMISSION_MODEL)
        return v.record_ && v.generation_ && v.epoch_ &&
            (v.record_->state==KernelEntryStackView::State::reserved || v.record_->state==KernelEntryStackView::State::active) &&
            v.record_->generation==v.generation_ && v.record_->epoch==v.epoch_;
#else
        (void)v;return false;
#endif
    }
    [[nodiscard]] static bool same(const KernelEntryStackView &a,const KernelEntryStackView &b) noexcept
    {
#if (defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__powerpc__) || defined(__PPC__)) && __SIZEOF_POINTER__ == 4 && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) || defined(OK_PPC32_ADMISSION_MODEL)
        return a.valid() && b.valid() && a.record_==b.record_ && a.generation_==b.generation_ && a.epoch_==b.epoch_;
#else
        (void)a;(void)b;return false;
#endif
    }
};
}
namespace ok::arch::execution::ppc32_private
{
inline constexpr u32 user_msr=0x0200c000;
inline constexpr usize entry_extent=752;
struct Facts { u32 pvr{},msr{},spefscr{}; bool spefscr_read{}; };
[[nodiscard]] constexpr bool qualified(Facts f) noexcept
{ return f.pvr==0x80210022 && f.msr==0 && f.spefscr_read && f.spefscr==0; }
[[nodiscard]] constexpr u32 fp_word(const UserFrame &f,usize at) noexcept
{ return (u32{f.fp_state[at]}<<24)|(u32{f.fp_state[at+1]}<<16)|(u32{f.fp_state[at+2]}<<8)|f.fp_state[at+3]; }
[[nodiscard]] constexpr bool native_word(u64 value) noexcept {return value<=0xffffffffULL;}
[[nodiscard]] constexpr bool frame_mode(const UserFrame &f,u32 program_cause=0) noexcept
{
    if(f.flags!=(user_msr|program_cause) || (program_cause&~0x000e0000U) || (fp_word(f,264)&0x00c00080) || !native_word(f.pc) ||
        !native_word(f.sp) || !native_word(f.fs_base) || !native_word(f.gs_base) ||
        f.registers[1]!=f.sp || f.registers[2]!=f.fs_base || f.registers[13]!=f.gs_base) return false;
    for(auto r:f.registers) if(!native_word(r)) return false;
    return true;
}
[[nodiscard]] constexpr bool same_space(AddressSpace a,AddressSpace b) noexcept
{ return a.root==b.root && a.slot==b.slot && a.generation==b.generation; }
[[nodiscard]] constexpr bool same_frame(const UserFrame &a,const UserFrame &b) noexcept
{
    if(a.pc!=b.pc || a.sp!=b.sp || a.flags!=b.flags || a.fs_base!=b.fs_base || a.gs_base!=b.gs_base) return false;
    for(usize i=0;i<a.registers.size();++i) if(a.registers[i]!=b.registers[i]) return false;
    for(usize i=0;i<a.fp_state.size();++i) if(a.fp_state[i]!=b.fp_state[i]) return false;
    return true;
}
enum class Event : u8 { none,syscall,timer,data_fault,instruction_fault,program,spe_unavailable,spe_data,spe_round };
enum DiagnosticValidity : u8 { original_r3_valid=1,result_valid=2,dar_valid=4,dsisr_valid=8 };
struct Diagnostics
{
    u32 original_r3{},trap{},dar{},dsisr{},result{};
    u32 raw_vector{},raw_esr{},raw_dear{},raw_srr1{};
    u8 validity{};
    Event event{};
    constexpr bool operator==(const Diagnostics &) const = default;
};
[[nodiscard]] constexpr Diagnostics diagnose(const UserFrame &f,const Trap &t) noexcept
{
    Diagnostics d{};
    d.raw_vector=t.vector;d.raw_esr=static_cast<u32>(t.error_code);d.raw_dear=static_cast<u32>(t.fault_address);d.raw_srr1=static_cast<u32>(f.flags);
    switch(t.vector)
    {
    case 8: if(t.kind!=TrapKind::syscall) return {};d.event=Event::syscall;d.trap=0xc00;d.original_r3=static_cast<u32>(f.registers[3]);d.validity=original_r3_valid;break;
    case 10: if(t.kind!=TrapKind::timer) return {};d.event=Event::timer;d.trap=0x900;break;
    // An external input preempts like the decrementer: resumable, nothing executed.
    case 4: if(t.kind!=TrapKind::interrupt) return {};d.event=Event::timer;d.trap=0x500;break;
    case 2: case 13:
        if(t.kind!=TrapKind::page_fault) return {};
        d.event=Event::data_fault;d.trap=0x300;d.dar=d.raw_dear;d.dsisr=d.raw_esr;d.validity=dar_valid|dsisr_valid;break;
    case 3: case 14:
        if(t.kind!=TrapKind::page_fault) return {};
        d.event=Event::instruction_fault;d.trap=0x400;d.dar=static_cast<u32>(f.pc);d.dsisr=0;d.validity=dar_valid|dsisr_valid;break;
    case 6:
        if(t.kind!=TrapKind::exception) return {};
        d.event=Event::program;d.trap=0x700;d.dsisr=d.raw_esr;d.validity=dsisr_valid;break;
    // Backend slots 16..18 are hardware IVOR32..34. Linux CONFIG_SPE mapping.
    case 16: if(t.kind!=TrapKind::exception) return {};d.event=Event::spe_unavailable;d.trap=0x2010;break;
    case 17: if(t.kind!=TrapKind::exception) return {};d.event=Event::spe_data;d.trap=0x2030;break;
    case 18: if(t.kind!=TrapKind::exception) return {};d.event=Event::spe_round;d.trap=0x2050;break;
    default: return {};
    }
    return d;
}
[[nodiscard]] constexpr bool event_mode(const UserFrame &f,const Diagnostics &d) noexcept
{
    // Program SRR1 carries PIL/PPR/PTR diagnostics as well as saved MSR.
    // This does not grant power-management or privilege authority on resume;
    // the actual native entry always writes its fixed user MSR.
    const u32 cause=d.event==Event::program?(d.raw_esr>>8)&0x000e0000U:0;
    return frame_mode(f,cause);
}
namespace detail { class Tracker;struct BackendAccess; }
class Capability final
{
    const detail::Tracker *issuer_{};
    u64 epoch_{},sequence_{};
    friend class detail::Tracker;
    friend class Observation;
  public:
    [[nodiscard]] bool admitted() const noexcept;
    [[nodiscard]] bool current() const noexcept;
    [[nodiscard]] constexpr u64 sequence() const noexcept {return sequence_;}
    [[nodiscard]] constexpr bool same_issuer(Capability other) const noexcept
    {return issuer_ && issuer_==other.issuer_ && epoch_==other.epoch_;}
};
class Observation final
{
    const detail::Tracker *issuer_{};
    const UserFrame *frame_{};
    u64 epoch_{},sequence_{};
    friend class detail::Tracker;
  public:
    [[nodiscard]] bool valid_for(const UserFrame &,AddressSpace,const KernelEntryStackView &) const noexcept;
    [[nodiscard]] bool from(Capability) const noexcept;
    [[nodiscard]] constexpr u64 sequence() const noexcept {return sequence_;}
    [[nodiscard]] Diagnostics diagnostics(const UserFrame &,AddressSpace,const KernelEntryStackView &) const noexcept;
};
namespace detail
{
class Tracker final
{
    bool admitted_{};
    u64 epoch_{},sequence_{};
    Observation observation_{};
    UserFrame frame_{};
    AddressSpace space_{};
    KernelEntryStackView view_{};
    Diagnostics diagnostics_{};
    uptr captured_sp_{};
    friend class ::ok::arch::execution::ppc32_private::Capability;
    friend class ::ok::arch::execution::ppc32_private::Observation;
    friend struct BackendAccess;
#ifdef OK_PPC32_ADMISSION_MODEL
    friend struct ModelAccess;
#endif
    constexpr Tracker() noexcept=default;
    void qualify_initialized(Facts facts) noexcept
    {
        invalidate();admitted_=false;
        if(epoch_==~u64{} || sequence_==~u64{}) return;
        ++epoch_;admitted_=qualified(facts);
    }
    void invalidate() noexcept
    {
        observation_={};view_={};diagnostics_={};captured_sp_=0;
        if(sequence_==~u64{}) admitted_=false;else ++sequence_;
    }
    void publish_owned(const UserFrame &frame,AddressSpace space,const KernelEntryStackView &view,const Trap &raw) noexcept
    {
        observation_={};view_={};diagnostics_={};captured_sp_=0;
        const auto diagnostics=diagnose(frame,raw);
        if(!admitted_ || !sequence_ || !event_mode(frame,diagnostics) || !space.root || !space.generation ||
            !view.valid() || !view.owner_pid() || !view.owner_tid() ||
            raw.kernel_stack_pointer!=view.top()-entry_extent ||
            !view.contains(raw.kernel_stack_pointer,entry_extent) || diagnostics.event==Event::none) return;
        frame_=frame;space_=space;view_=view;diagnostics_=diagnostics;captured_sp_=raw.kernel_stack_pointer;
        observation_.issuer_=this;observation_.frame_=&frame;observation_.epoch_=epoch_;observation_.sequence_=sequence_;
    }
  public:
    Tracker(const Tracker &)=delete;
    Tracker &operator=(const Tracker &)=delete;
    [[nodiscard]] Capability capability() const noexcept
    {Capability c;c.issuer_=this;c.epoch_=epoch_;c.sequence_=sequence_;return c;}
    [[nodiscard]] Observation query(const UserFrame &frame) const noexcept
    {return observation_.valid_for(frame,space_,view_)?observation_:Observation{};}
};
}
inline bool Capability::admitted() const noexcept
{return issuer_ && issuer_->admitted_ && epoch_==issuer_->epoch_;}
inline bool Capability::current() const noexcept
{return admitted() && sequence_==issuer_->sequence_;}
inline bool Observation::from(Capability cap) const noexcept
{return cap.current() && issuer_==cap.issuer_ && epoch_==cap.epoch_ && sequence_==cap.sequence_;}
inline bool Observation::valid_for(const UserFrame &frame,AddressSpace space,const KernelEntryStackView &view) const noexcept
{
    return issuer_ && issuer_->admitted_ && epoch_==issuer_->epoch_ && sequence_ &&
        sequence_==issuer_->sequence_ && issuer_->observation_.sequence_==sequence_ && frame_==&frame &&
        event_mode(frame,issuer_->diagnostics_) && same_frame(frame,issuer_->frame_) && same_space(space,issuer_->space_) &&
        Ppc32PrivateViewAccess::same(view,issuer_->view_) &&
        view.contains(issuer_->captured_sp_,entry_extent);
}
inline Diagnostics Observation::diagnostics(const UserFrame &frame,AddressSpace space,const KernelEntryStackView &view) const noexcept
{return valid_for(frame,space,view)?issuer_->diagnostics_:Diagnostics{};}
[[nodiscard]] Capability capability() noexcept;
[[nodiscard]] Observation observation(const UserFrame &) noexcept;
[[nodiscard]] Facts facts() noexcept;
[[nodiscard]] bool address_space_live(AddressSpace) noexcept;
} // namespace ok::arch::execution::ppc32_private
