#pragma once

#include "ok/user/native_signal_frame_mips_o32.hpp"

namespace ok::user::native {

// Supplied by the trusted backend/runtime, never decoded from user memory.
// Default construction does not attest hardware or a delay-slot continuation.
struct MipsO32SignalContext {
  using FpContract = MipsO32RtSignalFrame::FpContract;
  using ResumeContract = MipsO32RtSignalFrame::ResumeContract;
  FpContract fp{FpContract::unverified};
  ResumeContract resume{ResumeContract::unverified};

  template <MipsO32SignalFormat Format>
  constexpr typename MipsO32SignalFrame<Format>::Capture codec() const {
    using C = MipsO32SignalFrame<Format>;
    return {fp == FpContract::eager_fr0_legacy_nan_no_extensions
                ? C::FpContract::eager_fr0_legacy_nan_no_extensions
                : C::FpContract::unverified,
            resume == ResumeContract::normalized_resume_pc
                ? C::ResumeContract::normalized_resume_pc
                : C::ResumeContract::unverified};
  }
};

} // namespace ok::user::native
