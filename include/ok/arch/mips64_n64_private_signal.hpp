#pragma once

#include "ok/user/native_signal_config.hpp"
#include "ok/arch/execution.hpp"

namespace ok::arch::execution
{
struct Mips64N64PrivateViewAccess final
{
    [[nodiscard]] static u64 generation(const KernelEntryStackView &view) noexcept
    {
#if (defined(OK_NATIVE_SIGNAL_RUNTIME) && defined(__mips64)) || defined(OK_MIPS64_N64_ADMISSION_MODEL)
        return view.valid()?view.generation_:0;
#else
        (void)view;return 0;
#endif
    }
    [[nodiscard]] static bool same(const KernelEntryStackView &a,const KernelEntryStackView &b) noexcept
    {
#if (defined(OK_NATIVE_SIGNAL_RUNTIME) && defined(__mips64)) || defined(OK_MIPS64_N64_ADMISSION_MODEL)
        return a.valid() && b.valid() && a.record_==b.record_ && a.generation_==b.generation_ && a.epoch_==b.epoch_;
#else
        (void)a;(void)b;return false;
#endif
    }
};
}
namespace ok::arch::execution::mips64_n64_private
{
inline constexpr u32 user_status = 0x240080b3;
inline constexpr u32 unsupported_fcsr = 0x007c0000;
struct Facts
{
    u32 prid{}, config0{}, config1{}, config2{}, config3{}, config4{}, config5{};
    u32 status{}, fir{}, fcsr{};
    bool has_config1{}, has_config2{}, has_config3{}, has_config4{}, has_config5{};
};
[[nodiscard]] constexpr bool platform(const Facts &f) noexcept
{
    // FIR PS/3D use the retained scalar registers; DSP/MDMX/MSA do not.
    return f.prid == 0x00010000 && f.config0 == 0x8000c482 &&
        f.has_config1 && f.config1 == 0xfea3519b &&
        f.has_config2 && f.config2 == 0x80000000 &&
        f.has_config3 && f.config3 == 0x00000080 &&
        !f.has_config4 && !f.has_config5 && !f.config4 && !f.config5;
}
[[nodiscard]] constexpr bool qualified(const Facts &f) noexcept
{ return platform(f) && f.status == 0x24000080 && f.fir == 0x007f0000 && f.fcsr == 0; }
[[nodiscard]] constexpr u32 fcsr(const UserFrame &f) noexcept
{
    return (u32{f.fp_state[256]} << 24) | (u32{f.fp_state[257]} << 16) |
        (u32{f.fp_state[258]} << 8) | f.fp_state[259];
}
[[nodiscard]] constexpr bool frame_mode(const UserFrame &f) noexcept
{ return f.flags == user_status && !(fcsr(f) & unsupported_fcsr); }
[[nodiscard]] constexpr bool same_space(AddressSpace a, AddressSpace b) noexcept
{ return a.root == b.root && a.slot == b.slot && a.generation == b.generation; }
[[nodiscard]] constexpr bool same_frame(const UserFrame &a, const UserFrame &b) noexcept
{
    return a.registers == b.registers && a.pc == b.pc && a.sp == b.sp && a.flags == b.flags &&
        a.fs_base == b.fs_base && a.gs_base == b.gs_base && a.fp_state == b.fp_state;
}
[[nodiscard]] constexpr bool delay_operands_admitted(u32 insn) noexcept
{
    const u32 op=insn>>26, rs=(insn>>21)&31, rt=(insn>>16)&31, rd=(insn>>11)&31;
    const auto retained=[](u32 reg){return reg!=26 && reg!=27;};
    switch(op)
    {
    case 0:
        // Reject reserved fields and a JALR target destroyed by its link write.
        if((insn&63)==8) return retained(rs) && !(insn&0x001fffc0u);
        if((insn&63)==9) return retained(rs) && rs!=rd && !(insn&0x001f07c0u);
        return false;
    case 2: case 3: return true;
    case 4: case 5: case 20: case 21: return retained(rs) && retained(rt);
    case 6: case 7: case 22: case 23: return retained(rs) && rt==0;
    case 1:
        return retained(rs) && (rt==0 || rt==1 || rt==2 || rt==3 ||
            ((rt==16 || rt==17 || rt==18 || rt==19) && rs!=31));
    case 17: return rs==8; // BC1: captured scalar FCSR contains every FCC bit.
    default: return false;
    }
}
namespace detail { class Tracker; struct BackendAccess; }
class Capability final
{
    const detail::Tracker *issuer_{};
    u64 epoch_{}, sequence_{};
    friend class detail::Tracker;
    friend class Observation;
  public:
    [[nodiscard]] bool admitted() const noexcept;
    [[nodiscard]] bool current() const noexcept;
    [[nodiscard]] constexpr u64 sequence() const noexcept { return sequence_; }
    [[nodiscard]] constexpr bool same_issuer(Capability other) const noexcept
    { return issuer_ && issuer_==other.issuer_ && epoch_==other.epoch_; }
};
enum class CaptureKind : u8 { unverified, syscall_normalized, timer_non_bd, timer_bd };
class Observation final
{
    const detail::Tracker *issuer_{};
    const UserFrame *frame_{};
    u64 epoch_{}, sequence_{};
    CaptureKind kind_{};
    friend class detail::Tracker;
  public:
    [[nodiscard]] bool valid_for(const UserFrame &, AddressSpace, const KernelEntryStackView &) const noexcept;
    [[nodiscard]] bool from(Capability) const noexcept;
    [[nodiscard]] constexpr u64 sequence() const noexcept { return sequence_; }
    [[nodiscard]] constexpr CaptureKind kind() const noexcept { return kind_; }
};
namespace detail
{
// Construction is restricted to the backend; models use an explicitly named
// host-only friend. No ordinary runtime caller can mint a capability/observation.
class Tracker final
{
    bool admitted_{};
    u64 epoch_{}, sequence_{};
    Observation observation_{};
    UserFrame captured_{};
    AddressSpace space_{};
    KernelEntryStackView view_{};
    uptr captured_sp_{};
    friend class ::ok::arch::execution::mips64_n64_private::Capability;
    friend class ::ok::arch::execution::mips64_n64_private::Observation;
    friend struct BackendAccess;
#ifdef OK_MIPS64_N64_ADMISSION_MODEL
    friend struct ModelAccess;
#endif
    constexpr Tracker() noexcept = default;
  public:
    Tracker(const Tracker &) = delete;
    Tracker &operator=(const Tracker &) = delete;
    [[nodiscard]] bool qualify(const Facts &facts) noexcept
    {
        observation_={};view_={};admitted_=false;
        if(epoch_==~u64{}) return false;
        ++epoch_;admitted_=qualified(facts);return admitted_;
    }
    [[nodiscard]] Capability capability() const noexcept
    { Capability out;out.issuer_=this;out.epoch_=epoch_;out.sequence_=sequence_;return out; }
    void invalidate() noexcept
    {
        observation_={};view_={};captured_sp_=0;
        if(sequence_==~u64{}) admitted_=false;
        else ++sequence_;
    }
    void publish_owned(const UserFrame &frame, AddressSpace space, const KernelEntryStackView &view,
                       const Trap &trap) noexcept
    {
        observation_={};view_={};captured_sp_=0;
        if(!admitted_ || !sequence_ || !frame_mode(frame) || !space.generation ||
            !view.valid() || !view.owner_pid() || !view.owner_tid() ||
            !trap.kernel_stack_pointer || !view.contains(trap.kernel_stack_pointer, sizeof(UserFrame)+16)) return;
        CaptureKind kind=CaptureKind::unverified;
        if(trap.kind==TrapKind::syscall) kind=CaptureKind::syscall_normalized;
        else if(trap.kind==TrapKind::timer)
            kind=(static_cast<u32>(trap.error_code)&0x80000000u)?CaptureKind::timer_bd:CaptureKind::timer_non_bd;
        if(kind==CaptureKind::unverified) return;
        captured_=frame;space_=space;view_=view;captured_sp_=trap.kernel_stack_pointer;
        observation_.issuer_=this;observation_.frame_=&frame;observation_.epoch_=epoch_;
        observation_.sequence_=sequence_;observation_.kind_=kind;
    }
    [[nodiscard]] Observation query(const UserFrame &frame) const noexcept
    {
        return observation_.frame_==&frame && observation_.valid_for(frame,space_,view_) ? observation_ : Observation{};
    }
};
} // namespace detail
inline bool Capability::admitted() const noexcept
{ return issuer_ && issuer_->admitted_ && epoch_==issuer_->epoch_; }
inline bool Capability::current() const noexcept
{ return admitted() && sequence_==issuer_->sequence_; }
inline bool Observation::from(Capability cap) const noexcept
{ return cap.current() && issuer_==cap.issuer_ && epoch_==cap.epoch_; }
inline bool Observation::valid_for(const UserFrame &frame, AddressSpace space,
                                   const KernelEntryStackView &view) const noexcept
{
    return issuer_ && issuer_->admitted_ && epoch_==issuer_->epoch_ && sequence_!=0 &&
        sequence_==issuer_->sequence_ && issuer_->observation_.sequence_==sequence_ &&
        frame_==&frame && kind_!=CaptureKind::unverified &&
        same_frame(frame,issuer_->captured_) && frame_mode(frame) && same_space(space,issuer_->space_) &&
        Mips64N64PrivateViewAccess::same(view,issuer_->view_) && view.lower()==issuer_->view_.lower() &&
        view.top()==issuer_->view_.top() && view.owner_pid()==issuer_->view_.owner_pid() &&
        view.owner_tid()==issuer_->view_.owner_tid() &&
        view.contains(issuer_->captured_sp_,sizeof(UserFrame)+16);
}
[[nodiscard]] Capability capability() noexcept;
[[nodiscard]] Observation observation(const UserFrame &) noexcept;
[[nodiscard]] Facts facts() noexcept;
} // namespace ok::arch::execution::mips64_n64_private
