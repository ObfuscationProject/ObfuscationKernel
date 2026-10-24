#pragma once
namespace linux_n64_oracle {
inline constexpr unsigned long long frame_bytes = 808;
inline constexpr unsigned long long frame_type_alignment = 8;
inline constexpr unsigned long long context_bytes = 600;
inline constexpr unsigned long long uc_bytes = 656;
inline constexpr unsigned long long info = 24;
inline constexpr unsigned long long uc = 152;
inline constexpr unsigned long long uc_stack = 16;
inline constexpr unsigned long long uc_context = 40;
inline constexpr unsigned long long uc_mask = 640;
inline constexpr unsigned long long regs = 0;
inline constexpr unsigned long long fpregs = 256;
inline constexpr unsigned long long hi = 512;
inline constexpr unsigned long long lo = 544;
inline constexpr unsigned long long pc = 576;
inline constexpr unsigned long long fcsr = 584;
inline constexpr unsigned long long used = 588;
inline constexpr unsigned long long dsp = 592;
inline constexpr unsigned long long reserved = 596;
inline constexpr unsigned long long hi1 = 520;
inline constexpr unsigned long long lo3 = 568;
inline constexpr unsigned long long info_bytes = 128;
inline constexpr unsigned long long info_code = 4;
inline constexpr unsigned long long info_errno = 8;
inline constexpr unsigned long long info_pid = 16;
inline constexpr unsigned long long info_uid = 20;
inline constexpr unsigned long long stack_bytes = 24;
inline constexpr unsigned long long stack_size = 8;
inline constexpr unsigned long long stack_flags = 16;
inline constexpr unsigned long long mask_bytes = 16;
inline constexpr unsigned long long USED_FP = 1;
inline constexpr unsigned long long USED_FR1 = 2;
inline constexpr unsigned long long USED_HYBRID = 4;
inline constexpr unsigned long long USED_EXT = 8;
inline constexpr unsigned long long SIGSTOP = 23;
inline constexpr unsigned long long SIGFPE = 8;
}
