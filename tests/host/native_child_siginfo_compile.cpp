#include "ok/user/native_signal_info.hpp"
#include "../../src/user/signals/process_pending.hpp"
#include <cstddef>
using namespace ok;
using namespace ok::user::native;
#if defined(OK_ARCH_TARGET_I386)
#include "ok/user/native_signal_delivery_i386.hpp"
using Frame = I386RtSignalFrame;
constexpr auto selected = abi::linux_user::Profile::i386;
#elif defined(OK_ARCH_TARGET_X86_64)
#include "ok/user/native_signal_delivery_x86_64.hpp"
using Frame = X86SignalFrame;
constexpr auto selected = abi::linux_user::Profile::x86_64;
#elif defined(OK_ARCH_TARGET_AARCH64)
#include "ok/user/native_signal_delivery_aarch64.hpp"
using Frame = Aarch64SignalFrame;
constexpr auto selected = abi::linux_user::Profile::aarch64;
#elif defined(OK_ARCH_TARGET_ARM32)
#include "ok/user/native_signal_delivery_arm32.hpp"
using Frame = Arm32RtSignalFrame;
constexpr auto selected = abi::linux_user::Profile::arm32;
#elif defined(OK_ARCH_TARGET_RV32) || defined(OK_ARCH_TARGET_RV64)
#include "ok/user/native_signal_delivery_riscv.hpp"
#if defined(OK_ARCH_TARGET_RV32)
constexpr auto selected = abi::linux_user::Profile::rv32;
#else
constexpr auto selected = abi::linux_user::Profile::rv64;
#endif
using Frame = RiscvSignalFrame<selected>;
#elif defined(OK_ARCH_TARGET_LOONGARCH64)
#include "ok/user/native_signal_delivery_loongarch64.hpp"
using Frame = Loongarch64SignalFrame;
constexpr auto selected = abi::linux_user::Profile::loongarch64;
#elif defined(OK_ARCH_TARGET_MIPS)
#include "ok/user/native_signal_delivery_mips_o32.hpp"
using Frame = MipsO32RtSignalFrame;
constexpr auto selected = abi::linux_user::Profile::mips;
#elif defined(OK_ARCH_TARGET_MIPS64)
#include "ok/user/native_signal_delivery_mips64_n64.hpp"
using Frame = Mips64N64SignalFrame;
constexpr auto selected = abi::linux_user::Profile::mips64;
#elif defined(OK_ARCH_TARGET_PPC)
#include "ok/user/native_signal_delivery_ppc32.hpp"
using Frame = Ppc32RtSignalFrame;
constexpr auto selected = abi::linux_user::Profile::ppc;
#else
#error A retained architecture profile is required
#endif
using Codec = SignalInfoCodec<selected>;
using Queue = sched::PendingSignals<Codec::maximum_signal>;
using Shared = ProcessPendingSignals<Codec::maximum_signal>;
constexpr u32 child = Codec::maximum_signal == 128 ? 18 : 17;
constexpr auto payloads()
{
    std::array<std::byte, 9 * 128> result{};
    constexpr std::array<sched::PendingSignalInfo, 9> records{{
        {child, 1, 0x12345678, 0xfedcba98, 0}, {child, 1, 0x12345678, 0xfedcba98, 165},
        {child, 1, 0x12345678, 0xfedcba98, 255}, {child, 2, 0x12345678, 0xfedcba98, 1},
        {child, 2, 0x12345678, 0xfedcba98, Codec::maximum_signal},
        {child, 0, 0x12345678, 0xfedcba98}, {8, -6, 0x12345678, 0xfedcba98},
        {8, 128, 0, 0}, {Codec::maximum_signal, 128, 0, 0}}};
    for (usize i = 0; i != records.size(); ++i)
        if (!Codec::encode_into(records[i], result, i * 128)) return std::array<std::byte, 9 * 128>{};
    return result;
}
static_assert(Frame::valid_info({static_cast<i32>(child), 1, 1, 0, 165}));
static_assert(sizeof(Frame::Info) == 20 && sizeof(sched::PendingSignalInfo) == 20);
extern "C" [[gnu::used, gnu::section(".child_payloads")]] const auto child_payloads = payloads();
extern "C" [[gnu::used, gnu::section(".child_layout")]] const u32 child_layout[] = {
    sizeof(sched::PendingSignalInfo), alignof(sched::PendingSignalInfo), offsetof(sched::PendingSignalInfo, child_status),
    sizeof(Frame::Info), alignof(Frame::Info), offsetof(Frame::Info, child_status),
    sizeof(Queue), alignof(Queue), sizeof(Queue::Delivery), alignof(Queue::Delivery),
    sizeof(Shared), alignof(Shared), sizeof(Shared::Delivery), alignof(Shared::Delivery),
    sizeof(sched::ChildExitStatus), alignof(sched::ChildExitStatus)};
extern "C" bool child_signal_admission(Shared &queue, u32 pid, u32 uid)
{
    return queue.enqueue_child_exit(sched::ChildExitStatus::normal(165), pid, uid) == Shared::Enqueue::queued;
}
