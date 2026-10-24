#pragma once
#include "ok/arch/ppc32_private_signal.hpp"
#include "ok/abi/linux_native.hpp"
#include "ok/user/native_ppc32_elf_admission.hpp"
#include "ok/user/native_signal_context_ppc32.hpp"

namespace ok::user::native
{
// Metadata eligibility under the measured CPU and explicit ABI contract. This
// token neither loads the ELF nor makes an initial or cloned frame ready.
class Ppc32ImageAdmission final
{
    arch::execution::ppc32_private::Capability cpu_{};
    ppc32_elf_admission::Personality personality_{};
    bool admitted_{};
  public:
    [[nodiscard]] static Ppc32ImageAdmission inspect(
        arch::execution::ppc32_private::Capability cpu,std::span<const std::byte> prefix,u64 file_bytes,
        ppc32_elf_admission::Personality personality,ppc32_elf_admission::RuntimeProfile profile) noexcept
    {
        Ppc32ImageAdmission out;
        if(!cpu.current() || !ppc32_elf_admission::admit(prefix,file_bytes,personality,profile).ok()) return out;
        out.cpu_=cpu;out.personality_=personality;out.admitted_=true;return out;
    }
    [[nodiscard]] bool valid(arch::execution::ppc32_private::Capability cpu) const noexcept
    {return admitted_ && cpu.admitted() && cpu_.admitted() && cpu.same_issuer(cpu_);}
    [[nodiscard]] ppc32_elf_admission::Personality personality() const noexcept {return personality_;}
};

// A conservative single-native-entry proof window. Any subsequent native entry
// revokes the proof, including unrelated entries. Runtime wiring may not infer
// handler/exec/fork readiness from this object's prior state or frame shape.
class Ppc32SignalReadiness final
{
  public:
    using Frame=arch::execution::UserFrame;
    using Space=arch::execution::AddressSpace;
    using View=arch::execution::KernelEntryStackView;
    using Capability=arch::execution::ppc32_private::Capability;
    using Observation=arch::execution::ppc32_private::Observation;
    using Diagnostics=arch::execution::ppc32_private::Diagnostics;
    struct Identity
    {
        Space space{};
        u64 pid{},tid{},image_generation{};
        [[nodiscard]] constexpr bool valid() const noexcept {return space.root && space.generation && pid && tid && image_generation;}
        [[nodiscard]] constexpr bool operator==(Identity other) const noexcept
        {return arch::execution::ppc32_private::same_space(space,other.space) && pid==other.pid && tid==other.tid && image_generation==other.image_generation;}
    };
    enum class State : u8 { unverified,awaiting_capture,syscall_pending,captured,completed };
  private:
    Identity identity_{};
    Ppc32ImageAdmission image_{};
    State state_{};
    const Frame *frame_{};
    Frame saved_{};
    View view_{};
    u64 sequence_{};
    Diagnostics diagnostics_{};
    [[nodiscard]] bool owns(Identity id,Capability cap) const noexcept
    {return id.valid() && id==identity_ && image_.valid(cap) && cap.current() && arch::execution::ppc32_private::address_space_live(id.space);}
    [[nodiscard]] bool unchanged(Identity id,const Frame &frame,Capability cap) const noexcept
    {
        return owns(id,cap) && sequence_==cap.sequence() && frame_==&frame &&
            arch::execution::Ppc32PrivateViewAccess::retained(view_) &&
            arch::execution::ppc32_private::event_mode(frame,diagnostics_) &&
            arch::execution::ppc32_private::same_frame(saved_,frame);
    }
  public:
    constexpr Ppc32SignalReadiness() noexcept=default;
    Ppc32SignalReadiness(const Ppc32SignalReadiness &)=delete;
    Ppc32SignalReadiness &operator=(const Ppc32SignalReadiness &)=delete;
    [[nodiscard]] State state() const noexcept {return state_;}
    void revoke() noexcept {state_=State::unverified;frame_=nullptr;view_={};sequence_=0;diagnostics_={};}
    // Use for create/reuse/exec/fork even when a child shares an address space.
    // A copied ELF admission is eligibility only: every child needs a new trap.
    void reset(Identity id,Ppc32ImageAdmission image) noexcept {revoke();identity_=id.valid()?id:Identity{};image_=image;}
    [[nodiscard]] bool begin_entry(Identity id,const Frame &frame,Space space,const View &view,Capability cap) noexcept
    {
        revoke();
        if(!owns(id,cap) || !view.valid() || view.owner_pid()!=id.pid || view.owner_tid()!=id.tid ||
            !arch::execution::ppc32_private::same_space(space,id.space)) return false;
        state_=State::awaiting_capture;frame_=&frame;view_=view;sequence_=cap.sequence();return true;
    }
    // Must run while the exact activation remains live, before pool deactivation.
    [[nodiscard]] bool capture(Identity id,const Frame &frame,Space space,const View &view,Observation observation,Capability cap) noexcept
    {
        const bool valid=owns(id,cap) && state_==State::awaiting_capture && frame_==&frame &&
            arch::execution::Ppc32PrivateViewAccess::same(view_,view) && view.owner_pid()==id.pid && view.owner_tid()==id.tid &&
            arch::execution::ppc32_private::same_space(space,id.space) && observation.from(cap) &&
            observation.sequence()>sequence_ && observation.valid_for(frame,space,view);
        if(!valid) {revoke();return false;}
        diagnostics_=observation.diagnostics(frame,space,view);saved_=frame;sequence_=observation.sequence();
        state_=diagnostics_.event==arch::execution::ppc32_private::Event::syscall?State::syscall_pending:State::captured;
        return true;
    }
    [[nodiscard]] bool ready(Identity id,const Frame &frame,Capability cap) const noexcept
    {return (state_==State::captured || state_==State::completed) && unchanged(id,frame,cap);}
    [[nodiscard]] bool diagnostics(Identity id,const Frame &frame,Capability cap,Diagnostics &out) const noexcept
    {
        out={};
        if((state_!=State::syscall_pending && state_!=State::captured && state_!=State::completed) || !unchanged(id,frame,cap)) return false;
        out=diagnostics_;return true;
    }
    // Capture the semantic signed native32 result before the Linux bridge
    // encodes positive errno/CR0.SO. No caller-mutated frame is republished.
    [[nodiscard]] bool complete_syscall(Identity id,Frame &frame,Capability cap,abi::linux_user::Return result) noexcept
    {
        if(state_!=State::syscall_pending || !unchanged(id,frame,cap) ||
            image_.personality()!=ppc32_elf_admission::Personality::linux_user ||
            diagnostics_.event!=arch::execution::ppc32_private::Event::syscall ||
            diagnostics_.validity!=arch::execution::ppc32_private::original_r3_valid) return false;
        const u32 raw_result=result.error?u32{0}-result.error:static_cast<u32>(result.value);
        Frame completed=frame;
        if(!abi::linux_user::NativeBridge<abi::linux_user::Profile::ppc>::encode_return(completed,result).ok()) return false;
        diagnostics_.result=raw_result;diagnostics_.validity|=arch::execution::ppc32_private::result_valid;
        frame=completed;saved_=completed;state_=State::completed;return true;
    }
    [[nodiscard]] Ppc32SignalContext context(Identity id,const Frame &frame,Capability cap) const noexcept
    {
        Ppc32SignalContext out{};
        if(!ready(id,frame,cap) || image_.personality()!=ppc32_elf_admission::Personality::linux_user) return out;
        out.hardware=Ppc32SignalContext::Hardware::native32_be_e500v2_eager_spe_only;
        out.resume=Ppc32SignalContext::Resume::normalized_no_restart;
        out.metadata={diagnostics_.original_r3,diagnostics_.trap,diagnostics_.dar,diagnostics_.dsisr,diagnostics_.result};
        out.metadata_supplied=true;return out;
    }
};
} // namespace ok::user::native
