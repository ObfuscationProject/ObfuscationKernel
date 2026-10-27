#include "ok/arch/execution.hpp"
#include "ok/memory/kernel_entry_stack_pool.hpp"
#include "ok/abi/linux_native.hpp"
#include "ok/arch/ppc/signal.hpp"

using namespace ok;
namespace native = ok::arch::execution;
namespace ppc_private=ok::arch::execution::ppc32_private;
using Readiness=ok::user::native::Ppc32SignalReadiness;
using ImageAdmission=ok::user::native::Ppc32ImageAdmission;
ImageAdmission image_admission;
Readiness admission_readiness;
extern "C" const std::byte ok_ppc_admission_elf_begin[],ok_ppc_admission_elf_end[];

extern "C"
{
    extern const u8 ok_native_user_begin[], ok_native_user_end[];
    extern const u8 ok_ppc_program_illegal[],ok_ppc_program_privileged[],ok_ppc_program_trap[];
    extern const u8 ok_audit_capture[], ok_audit_loop[], ok_audit_usprg[], ok_audit_save[];
    extern const u8 ok_native_user_kernel_read[], ok_native_user_readonly_write[];
    extern const u8 ok_native_user_tlb_stress[];
    extern const u8 ok_native_user_nx[], ok_native_user_unmapped[], ok_native_user_invalid[];
}
namespace
{
alignas(4096) u8 code[4096]{};
alignas(4096) u8 stress_pages[20][4096]{};
alignas(4096) u8 data_a[4096]{};
alignas(4096) u8 data_b[4096]{};
alignas(4096) u8 stack_a[4096]{};
alignas(4096) u8 stack_b[4096]{};
constexpr uptr code_va = native::user_begin;
constexpr uptr data_va = native::user_begin + 8192;
constexpr uptr stack_va = native::user_end - 4096;

void print(std::string_view text)
{
    auto *uart = reinterpret_cast<volatile u8 *>(0xe0004500);
    for (const auto value : text)
    {
        while ((uart[5] & 0x20) == 0)
        {
        }
        uart[0] = static_cast<u8>(value);
    }
}
[[noreturn]] void finish(bool)
{
    for (;;)
        asm volatile("wrteei 0; b ." : : : "memory");
}
void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        print("OK_NATIVE_TEST_FAIL ");
        print(message);
        print("\n");
        finish(false);
    }
}
alignas(4096) std::byte owned_arena[memory::KernelEntryStackPool::arena_bytes]{};
memory::KernelEntryStackPool owned_pool;
memory::KernelEntryStackHandle owned_handle;
constexpr memory::KernelEntryStackOwner owned_owner{1, 2};
u32 owned_entries{};
void number(u64 value, unsigned width=8)
{
    constexpr char digits[]="0123456789abcdef";
    char text[16]{};
    for(unsigned i=0;i<width;++i)text[i]=digits[(value>>(4*(width-1-i)))&15];
    print(std::string_view{text,width});
}
void field(std::string_view name,u64 value,unsigned width=8){print(" ");print(name);print("=");number(value,width);}
u32 msr(){u32 v;asm volatile("mfmsr %0":"=r"(v));return v;}
template<unsigned Spr>u32 spr(){u32 v;asm volatile("mfspr %0,%1":"=r"(v):"i"(Spr));return v;}
u64 get(const native::UserFrame &f,usize at,usize n=4){u64 v{};for(usize i=0;i<n;++i)v=(v<<8)|f.fp_state[at+i];return v;}
void put(native::UserFrame &f,usize at,u64 v,usize n=4){for(usize i=0;i<n;++i)f.fp_state[at+i]=v>>(8*(n-1-i));}
Result<native::Trap> owned_enter(native::AddressSpace &space,native::UserFrame &frame)
{
    const auto active=owned_pool.activate_entry(owned_handle,owned_owner,0);
    require(active.ok(),"mint active owned entry");
    const auto view=active.value();
    const Readiness::Identity id{space,owned_owner.pid,owned_owner.tid,1};
    admission_readiness.reset(id,image_admission);
    require(!admission_readiness.ready(id,frame,ppc_private::capability()),"metadata alone never publishes readiness");
    require(admission_readiness.begin_entry(id,frame,space,view,ppc_private::capability()),"begin exact private owned capture");
    auto result=native::enter_owned(space,frame,view);
    require(result.ok(),"owned native enter");
    require(result.value().kernel_stack_pointer==view.top()-752,"exact actual owned privileged stack capture");
    require(view.contains(result.value().kernel_stack_pointer,752),"owned privileged capture extent");
    const auto observed=ppc_private::observation(frame);
    if(!observed.valid_for(frame,space,view))
    {print("PPC_ADMISSION_REJECT_CAPTURE");field("vector",result.value().vector);field("flags",frame.flags);field("spefscr",get(frame,264));field("r1",frame.registers[1]);field("sp",frame.sp);field("r2",frame.registers[2]);field("fs",frame.fs_base);field("r13",frame.registers[13]);field("gs",frame.gs_base);print("\n");}
    require(observed.valid_for(frame,space,view),"backend opaque observation matches actual owned event");
    require(admission_readiness.capture(id,frame,space,view,observed,ppc_private::capability()),"consume actual owned observation");
    ppc_private::Diagnostics diagnostic;
    require(admission_readiness.diagnostics(id,frame,ppc_private::capability(),diagnostic),"trusted diagnostics available before dispatch");
    require(diagnostic.raw_vector==result.value().vector,"real event vector provenance");
    if(result.value().kind==native::TrapKind::syscall)
        require(diagnostic.original_r3==frame.registers[3] && diagnostic.trap==0xc00 && diagnostic.validity==ppc_private::original_r3_valid && !admission_readiness.ready(id,frame,ppc_private::capability()),"syscall orig captured before completion");
    if(result.value().kind==native::TrapKind::timer)
        require(diagnostic.trap==0x900 && !diagnostic.original_r3 && !diagnostic.result && !diagnostic.dar && !diagnostic.dsisr && !diagnostic.validity,"timer diagnostic validity deterministic");
    if(result.value().vector==3 || result.value().vector==14)
        require(diagnostic.trap==0x400 && diagnostic.dar==frame.pc && diagnostic.dsisr==0,"instruction diagnostic ESR normalized");
    print("PPC_ADMISSION_EVENT");field("vector",diagnostic.raw_vector);field("linux_trap",diagnostic.trap);field("orig_r3",diagnostic.original_r3);field("dsisr",diagnostic.dsisr);field("raw_esr",diagnostic.raw_esr);field("raw_srr1",diagnostic.raw_srr1);field("validity",diagnostic.validity);print("\n");
    require(owned_pool.deactivate_entry(owned_handle,owned_owner,0).ok(),"revoke owned entry");
    require(!observed.valid_for(frame,space,view),"deactivation revokes unconsumed backend observation");
    require(admission_readiness.diagnostics(id,frame,ppc_private::capability(),diagnostic),"consumed private diagnostic survives deactivation");
    require(!view.valid(),"stale view invalidated");
    ++owned_entries;
    return result;
}
native::Trap next_non_timer(native::AddressSpace &space, native::UserFrame &frame)
{
    for (usize attempt = 0; attempt < 128; ++attempt)
    {
        const auto result = owned_enter(space, frame);
        require(result.ok(), "enter returned an error");
        if (result.value().kind != native::TrapKind::timer)
            return result.value();
    }
    require(false, "no forward progress after timer preemption");
    return {};
}
void check_fault(native::AddressSpace &space, const u8 *entry, native::TrapKind kind, uptr address, uptr error_mask)
{
    auto frame = native::initial_frame(code_va + static_cast<uptr>(entry - ok_native_user_begin), native::user_end);
    const auto trap = next_non_timer(space, frame);
    print("PPC_AUDIT_FAULT");field("entry",static_cast<uptr>(entry-ok_native_user_begin));field("vector",trap.vector);field("esr",trap.error_code);field("dear",trap.fault_address);field("pc",frame.pc);print("\n");
    require(trap.kind == kind, "unexpected CPU fault kind");
    if (entry == ok_native_user_nx)
        require(trap.vector == 3, "hardware UX permission fault after readable data TLB fill");
    if (kind == native::TrapKind::page_fault)
        require(trap.fault_address == address, "unexpected hardware fault address");
    require((trap.error_code & error_mask) == error_mask, "missing hardware page-fault error bits");
}
native::UserFrame seed(const u8 *entry,u32 tag)
{
    auto f=native::initial_frame(code_va+static_cast<uptr>(entry-ok_native_user_begin),native::user_end);
    for(usize i=0;i<32;++i){f.registers[i]=tag+0x10001*i;put(f,8*i,0x80000000+tag+0x20003*i);put(f,8*i+4,f.registers[i]);}
    f.registers[1]=f.sp;
    f.registers[2]=f.fs_base=data_va+64;
    f.registers[13]=f.gs_base=data_va+128;
    put(f,256,0xa0b1c2d3e4f50617ULL+tag,8);
    put(f,264,2);put(f,272,0x12345678+tag);put(f,276,0x56789abc+tag);
    put(f,280,0xb468ace1);put(f,284,0xe0000045);put(f,288,0x78563412+tag);
    return f;
}
void check_state(const native::UserFrame &actual,const native::UserFrame &expected)
{
    for(usize i=0;i<32;++i)
    {
        if(actual.registers[i]!=expected.registers[i]){print("PPC_AUDIT_GPR_MISMATCH");field("r",i);field("actual",actual.registers[i]);field("expected",expected.registers[i]);print("\n");}
        require(actual.registers[i]==expected.registers[i],"every scalar GPR retained");
        if(get(actual,8*i)!=get(expected,8*i)){print("PPC_AUDIT_UPPER_MISMATCH");field("r",i);field("actual",get(actual,8*i));field("expected",get(expected,8*i));print("\n");}
        require(get(actual,8*i)==get(expected,8*i),"every SPE upper half retained");
    }
    require(actual.sp==expected.sp&&actual.fs_base==expected.fs_base&&actual.gs_base==expected.gs_base,"SP and TLS aliases retained");
    require(get(actual,256,8)==get(expected,256,8),"entire ACC retained");
    for(usize at=264;at<=288;at+=4)
        require(get(actual,at)==get(expected,at),"SPEFSCR LR CTR CR XER USPRG0 retained");
    require(actual.flags==0x0200c000,"actual user SRR1 SPE EE PR and no extra privilege bits");
}
void full_state_audit(native::AddressSpace &space)
{
    auto saved=seed(ok_audit_save,0x12300000);
    const auto save_expected=seed(ok_audit_save,0x67408000);
    auto save_trap=next_non_timer(space,saved);
    require(save_trap.vector==8,"independent user state write syscall");check_state(saved,save_expected);
    const auto save_pc=saved.pc;
    save_trap=next_non_timer(space,saved);
    require(save_trap.vector==8&&saved.pc==save_pc+4,"captured user state reentry syscall");check_state(saved,save_expected);
    print("PPC_AUDIT_INDEPENDENT_USER_WRITE_PASS scalar_gprs=32 spe_uppers=32 acc=1 controls=6\n");
    auto f=seed(ok_audit_capture,0x23004000);
    auto expected=f;
    const u32 start=f.pc;
    const u32 kernel_msr=msr(),kernel_usprg=spr<256>(),kernel_spefscr=spr<512>();
    auto trap=next_non_timer(space,f);
    require(trap.vector==8&&trap.kind==native::TrapKind::syscall,"audit hardware sc event");
    require(f.pc==start+4,"hardware syscall SRR0 already contains next PC");
    check_state(f,expected);
    require(msr()==kernel_msr&&spr<256>()==kernel_usprg&&spr<512>()==kernel_spefscr,"privileged MSR USPRG0 SPEFSCR restored");
    print("PPC_AUDIT_SC");field("pvr",spr<287>());field("kernel_msr",kernel_msr);field("user_msr",f.flags);field("kernel_spefscr",kernel_spefscr);field("user_spefscr",get(f,264));field("pc",f.pc);field("sc_pc",start);field("vector",trap.vector);field("esr",trap.error_code);field("dear",trap.fault_address);field("owned_sp",trap.kernel_stack_pointer);print("\n");
    for(usize i=0;i<32;++i){print("PPC_AUDIT_GPR");field("r",i);field("scalar",f.registers[i]);field("upper",get(f,8*i));print("\n");}
    print("PPC_AUDIT_SPECIAL");field("acc",get(f,256,8),16);field("lr",get(f,272));field("ctr",get(f,276));field("cr",get(f,280));field("xer",get(f,284));field("usprg0",get(f,288));print("\n");
    using Bridge=abi::linux_user::NativeBridge<abi::linux_user::Profile::ppc>;
    require(Bridge::encode_return(f,{0x71526384,0}).ok(),"ordinary successful completion");
    expected.registers[3]=0x71526384;put(expected,280,get(expected,280)&~0x10000000U);
    trap=next_non_timer(space,f);require(trap.vector==8&&f.pc==start+8,"success resume next sc");check_state(f,expected);
    print("PPC_AUDIT_SUCCESS");field("r3",f.registers[3]);field("cr",get(f,280));field("pc",f.pc);print("\n");
    require(Bridge::encode_return(f,{0,22}).ok(),"ordinary errno completion");
    expected.registers[3]=22;put(expected,280,get(expected,280)|0x10000000U);
    trap=next_non_timer(space,f);require(trap.vector==8&&f.pc==start+12,"errno resume next sc");check_state(f,expected);
    print("PPC_AUDIT_ERRNO");field("r3",f.registers[3]);field("cr",get(f,280));field("pc",f.pc);print("\n");
    // This is direct entry-state integrity, not execution of a signal-return syscall.
    f=seed(ok_audit_capture,0x34105000);put(f,280,0x5a5aa5a5);f.flags=0xffffffff;expected=f;
    trap=next_non_timer(space,f);require(trap.vector==8&&f.pc==start+4,"edited full frame direct entry");check_state(f,expected);
    print("PPC_AUDIT_EDITED_ENTRY");field("r0",f.registers[0]);field("r3",f.registers[3]);field("cr",get(f,280));field("input_msr",expected.flags);field("actual_msr",f.flags);print("\n");
    f=seed(ok_audit_loop,0x45206000);expected=f;
    for(unsigned i=0;i<4;++i)
    {
        const auto event=owned_enter(space,f);trap=event.value();
        require(trap.kind==native::TrapKind::timer&&trap.vector==10,"hardware decrementer full-state event");
        require(f.pc==expected.pc,"timer interrupted exact register-invariant branch PC");check_state(f,expected);
        print("PPC_AUDIT_TIMER");field("iteration",i);field("pc",f.pc);field("vector",trap.vector);field("esr",trap.error_code);field("dear",trap.fault_address);field("r3",f.registers[3]);field("cr",get(f,280));field("owned_sp",trap.kernel_stack_pointer);print("\n");
    }
    f=seed(ok_audit_usprg,0x56307000);expected=f;
    trap=next_non_timer(space,f);
    require(trap.vector==8&&f.registers[10]==get(expected,288)&&get(f,288)==expected.registers[9],"user USPRG0 readable and writable");
    require(spr<256>()==kernel_usprg,"user USPRG0 write does not escape kernel restoration");
    print("PPC_AUDIT_USPRG0");field("before",get(expected,288));field("read",f.registers[10]);field("user_write",get(f,288));field("kernel",spr<256>());print("\n");
    print("PPC_AUDIT_FULL_STATE_PASS scalar_gprs=32 spe_uppers=32 timers=4\n");
}
} // namespace

extern "C" void ok_platform_console_write_char(char value)
{
    print(std::string_view(&value, 1));
}
extern "C" void *memset(void *destination, int value, usize count)
{
    auto *bytes = static_cast<u8 *>(destination);
    for (usize i = 0; i < count; ++i)
        bytes[i] = static_cast<u8>(value);
    return destination;
}
extern "C" void *memcpy(void *destination, const void *source, usize count)
{
    auto *out_bytes = static_cast<u8 *>(destination);
    const auto *in_bytes = static_cast<const u8 *>(source);
    for (usize i = 0; i < count; ++i)
        out_bytes[i] = in_bytes[i];
    return destination;
}

extern "C" [[noreturn]] void kernel_main()
{
    print("OK_NATIVE_TEST boot=begin\n");
    print("PPC_AUDIT_BOOT");field("pvr",spr<287>());field("msr",msr());print("\n");
    require(!ppc_private::capability().admitted(),"supported alone grants no private capability");
    const auto initialized=native::initialize();
    print("PPC_ADMISSION_CPU");field("initialized",initialized.ok());field("capability",ppc_private::capability().admitted());field("pvr",ppc_private::facts().pvr);print("\n");
#ifdef OK_PPC_AUDIT_INIT_ONLY
    print(initialized.ok()?"PPC_AUDIT_INIT_ACCEPT\n":"PPC_AUDIT_INIT_REJECT\n");
    finish(initialized.ok());
#endif
    require(initialized.ok(), "native initialize");
    require(ppc_private::capability().admitted(),"measured private e500v2 capability");
    image_admission=ImageAdmission::inspect(ppc_private::capability(),
        {ok_ppc_admission_elf_begin,static_cast<usize>(ok_ppc_admission_elf_end-ok_ppc_admission_elf_begin)},
        static_cast<u64>(ok_ppc_admission_elf_end-ok_ppc_admission_elf_begin),
        user::ppc32_elf_admission::Personality::linux_user,user::ppc32_elf_admission::RuntimeProfile::e500v2_scalar_soft_float);
    require(image_admission.valid(ppc_private::capability()),"actual archived Linux ELF metadata admitted under explicit profile");
    require(owned_pool.activate(owned_arena).ok(),"activate actual owned stack pool");
    const auto lease=owned_pool.reserve(owned_owner);require(lease.ok(),"reserve actual owned stack lease");owned_handle=lease.value();
#ifdef OK_NATIVE_TEST_KERNEL_FAULT
    asm volatile(".long 0");
#endif
    require(static_cast<usize>(ok_native_user_end - ok_native_user_begin) < sizeof(code), "user test text capacity");
    memcpy(code, ok_native_user_begin, ok_native_user_end - ok_native_user_begin);
    const native::PageMapping mappings_a[] = {
        {code_va, reinterpret_cast<uptr>(code), native::page_read | native::page_execute},
        {data_va, reinterpret_cast<uptr>(data_a), native::page_read | native::page_write},
        {stack_va, reinterpret_cast<uptr>(stack_a), native::page_read | native::page_write},
    };
    const native::PageMapping mappings_b[] = {
        {code_va, reinterpret_cast<uptr>(code), native::page_read | native::page_execute},
        {data_va, reinterpret_cast<uptr>(data_b), native::page_read | native::page_write},
        {stack_va, reinterpret_cast<uptr>(stack_b), native::page_read | native::page_write},
    };
    auto result_a = native::create_space(mappings_a);
    auto result_b = native::create_space(mappings_b);
    require(result_a.ok() && result_b.ok(), "create hardware address spaces");
    auto space_a = result_a.value();
    auto space_b = result_b.value();
    auto completion_frame=seed(ok_audit_capture,0x11220000);
    const auto completion_trap=next_non_timer(space_a,completion_frame);
    require(completion_trap.kind==native::TrapKind::syscall,"private completion actual syscall");
    const Readiness::Identity completion_id{space_a,owned_owner.pid,owned_owner.tid,1};
    const auto original_r3=completion_frame.registers[3];
    require(admission_readiness.complete_syscall(completion_id,completion_frame,ppc_private::capability(),{0,22}),"private semantic syscall completion");
    ppc_private::Diagnostics completed;
    require(admission_readiness.diagnostics(completion_id,completion_frame,ppc_private::capability(),completed),"completed signed result provenance");
    require(completed.original_r3==original_r3 && completed.result==0xffffffea && completed.validity==3 && completion_frame.registers[3]==22 && (get(completion_frame,280)&0x10000000),"raw negative result precedes positive errno encoding");
    require(admission_readiness.context(completion_id,completion_frame,ppc_private::capability()).metadata_supplied,"context authorized only after real completed event");
    require(!admission_readiness.complete_syscall(completion_id,completion_frame,ppc_private::capability(),{7,0}),"completion consumed once");
    completion_frame.registers[4]^=1;
    require(!admission_readiness.ready(completion_id,completion_frame,ppc_private::capability()),"edited frame revokes readiness");
    admission_readiness.revoke();
    print("PPC_ADMISSION_COMPLETION_PASS orig=retained raw_result=ffffffea user_errno=00000016\n");
    full_state_audit(space_a);
    require(!native::user_range(space_a, 0x100000, 1), "reject kernel pointer");
    require(!native::user_range(space_a, data_va + 4095, 2), "reject partially unmapped buffer");
    require(!native::user_range(space_a, native::user_end - 1, static_cast<usize>(-1)), "reject overflowed buffer");
    require(!native::user_range(space_a, code_va, 1, true), "reject write to code");
    const u8 marker = 0xa5;
    require(native::copy_to_user(space_a, data_va + 128, &marker, 1).ok() && data_a[128] == marker,
            "copy user data through safe physical alias");
    print("OK_NATIVE_TEST user_copy=pass\n");

    auto frame_a = native::initial_frame(code_va, native::user_end);
    frame_a.fs_base = data_va + 64;
    frame_a.gs_base = data_va + 128;
    asm volatile(".machine e500; mfmsr 0; oris 0,0,0x0200; mtmsr 0; isync; "
                 "lis 0,0xa6b7; ori 0,0,0xc8d9; evmergelo 14,0,14; evmra 0,14"
                 :
                 :
                 : "r0", "memory");
    const u32 sprg_marker = 0x1badb002;
    asm volatile("mtspr 272,%0; mtspr 256,%0" : : "r"(sprg_marker) : "memory");
    auto trap = next_non_timer(space_a, frame_a);
    require(trap.kind == native::TrapKind::syscall, "PR=1 SC trap");
    alignas(16) u8 kernel_spe[16]{};
    asm volatile("evstdd 14,0(%0); evxor 0,0,0; evmwumiaa 0,0,0; evstdd 0,8(%0)" : : "b"(kernel_spe) : "r0", "memory");
    require(kernel_spe[0] == 0xa6 && kernel_spe[8] == 0xa6, "kernel extended GPR and accumulator restored before C++");
    u32 restored_sprg;
    asm volatile("mfspr %0,272" : "=r"(restored_sprg));
    require(restored_sprg == sprg_marker, "kernel SPRG restored");
    asm volatile("mfspr %0,256" : "=r"(restored_sprg));
    require(restored_sprg == sprg_marker, "kernel USPRG0 restored");
    require(data_a[67] == 0x67 && data_a[131] == 0x54, "actual user r2/r13 TLS state");
    const auto call = native::syscall(frame_a);
    require(call.number == 0x1234 && call.arguments[0] == 0x23 && call.arguments[1] == 42,
            "actual CPL3 and syscall registers");
    require(frame_a.registers[14] == 0x11223344 && frame_a.registers[31] == 0x88776655, "callee-saved user registers");
    require((frame_a.flags & 0x4000) != 0, "actual user PR bit");
    require(frame_a.fp_state[112] == 0x55 && frame_a.fp_state[256] == 0x55, "SPE upper GPR and accumulator saved");
    require(frame_a.fp_state[267] == 2, "SPEFSCR saved");
    require(frame_a.fp_state[276] == 0x12 && frame_a.fp_state[280] == 0x24, "CTR and CR preserved");
    print("OK_NATIVE_TEST pr1=pass syscall=pass registers=pass\n");
    native::set_return_value(frame_a, 7);
    trap = next_non_timer(space_a, frame_a);
    require(trap.kind == native::TrapKind::syscall && native::syscall(frame_a).number == 0x2345,
            "syscall return resumes user instruction");
    require(data_a[3] == 7 && data_a[16] == 0x55 && data_a[24] == 0x55 && data_a[43] == 0x9a,
            "user return register and SPE state restored");

    auto frame_b = native::initial_frame(code_va, native::user_end);
    frame_b.fs_base = data_va + 64;
    frame_b.gs_base = data_va + 128;
    trap = next_non_timer(space_b, frame_b);
    require(trap.kind == native::TrapKind::syscall, "second process syscall");
    native::set_return_value(frame_b, 11);
    trap = next_non_timer(space_b, frame_b);
    require(trap.kind == native::TrapKind::syscall && data_b[3] == 11 && data_a[3] == 7,
            "same virtual address has distinct physical process storage");
    print("OK_NATIVE_TEST address_spaces=pass spe=pass kernel_context=pass\n");
    auto timer = owned_enter(space_a, frame_a);
    require(timer.ok() && timer.value().kind == native::TrapKind::timer && timer.value().vector == 10,
            "hardware decrementer preempts non-cooperative user loop");
    print("OK_NATIVE_TEST timer_preemption=pass\n");

    check_fault(space_a, ok_native_user_kernel_read, native::TrapKind::page_fault, 0x01000000, 0);
    check_fault(space_a, ok_native_user_readonly_write, native::TrapKind::page_fault, code_va, 0x00800000);
    check_fault(space_a, ok_native_user_nx, native::TrapKind::page_fault, data_va, 0);
    check_fault(space_a, ok_native_user_unmapped, native::TrapKind::page_fault, 0x60000000, 0);
    check_fault(space_a, ok_native_user_invalid, native::TrapKind::exception, 0, 0);
    check_fault(space_a,ok_ppc_program_illegal,native::TrapKind::exception,0,0x08000000);
    check_fault(space_a,ok_ppc_program_privileged,native::TrapKind::exception,0,0x04000000);
    check_fault(space_a,ok_ppc_program_trap,native::TrapKind::exception,0,0x02000000);
    print("OK_NATIVE_TEST supervisor_isolation=pass readonly=pass nx=pass faults=pass\n");
    native::PageMapping stress_mappings[22]{};
    stress_mappings[0] = mappings_a[0];
    stress_mappings[1] = mappings_a[2];
    for (usize i = 0; i < 20; ++i)
        stress_mappings[i + 2] = {0x41000000 + i * 4096, reinterpret_cast<uptr>(stress_pages[i]),
                                  native::page_read | native::page_write};
    auto stress_result = native::create_space(stress_mappings);
    require(stress_result.ok(), "create TLB eviction address space");
    auto stress_space = stress_result.value();
    auto stress_frame =
        native::initial_frame(code_va + (ok_native_user_tlb_stress - ok_native_user_begin), native::user_end);
    trap = next_non_timer(stress_space, stress_frame);
    require(trap.kind == native::TrapKind::syscall && native::syscall(stress_frame).number == 0x6789 &&
                stress_frame.registers[6] == 1,
            "transparent refill after TLB cache eviction");
    for (usize i = 0; i < 20; ++i)
        require(stress_pages[i][3] == i + 1, "every TLB cache mapping wrote its physical page");
    require(native::destroy_space(stress_space).ok(), "release TLB eviction address space");
    print("OK_NATIVE_TEST tls=pass tlb_eviction=pass\n");
    require(native::destroy_space(space_a).ok() && native::destroy_space(space_b).ok(),
            "release native address spaces");
    require(!native::user_range(space_a, data_va, 1), "stale address-space handle rejected");
    require(owned_pool.release(owned_handle,owned_owner).ok(),"release owned lease");
    require(owned_pool.accounting().leased_slots==0&&owned_pool.accounting().active_slots==0&&owned_pool.accounting().quarantined_slots==0,"owned accounting clean");
    print("PPC_AUDIT_OWNED");field("entries",owned_entries);print(" released=1 quarantine=0\n");
    print("PPC_ADMISSION_PASS scope=one-cpu-qemu-system runtime=not-integrated\n");
    print("OK_NATIVE_TEST_PASS scope=hardware architecture=ppc\n");
    finish(true);
}
