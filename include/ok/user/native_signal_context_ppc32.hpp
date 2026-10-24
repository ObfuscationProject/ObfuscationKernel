#pragma once

#include "ok/user/native_signal_frame_ppc32.hpp"

namespace ok::user::native
{

// Trusted runtime input. User MSR/SPEFSCR values cannot attest eager ownership,
// the e500v2 SPE-only profile, or normalized syscall/exception continuation.
struct Ppc32SignalContext
{
    using Hardware = Ppc32LegacySignalFrame::Hardware;
    using Resume = Ppc32LegacySignalFrame::Resume;
    using Metadata = Ppc32LegacySignalFrame::LinuxTrapMetadata;
    Hardware hardware{Hardware::unverified};
    Resume resume{Resume::unverified};
    Metadata metadata{};
    bool metadata_supplied{};

    template <Ppc32SignalFormat Format>
    [[nodiscard]] constexpr typename Ppc32SignalFrame<Format>::Capture codec() const
    {
        using C = Ppc32SignalFrame<Format>;
        return {{hardware == Hardware::native32_be_e500v2_eager_spe_only
                     ? C::Hardware::native32_be_e500v2_eager_spe_only : C::Hardware::unverified,
                 resume == Resume::normalized_no_restart
                     ? C::Resume::normalized_no_restart :
                 resume == Resume::prepared_syscall_restart
                     ? C::Resume::prepared_syscall_restart : C::Resume::unverified},
                {metadata.original_r3, metadata.trap, metadata.dar, metadata.dsisr, metadata.result},
                metadata_supplied};
    }
};

} // namespace ok::user::native
