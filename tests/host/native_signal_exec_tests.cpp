#include "unavailable_realtime.hpp"
// Hosted runtime integration: no guest CPU execution.
#include "native_runtime_units.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <tuple>
using namespace ok;
namespace native = ok::user::native;
namespace hw = ok::arch::execution;
namespace lu = ok::abi::linux_user;
#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(x))                                                                                                      \
        {                                                                                                              \
            std::fprintf(stderr, "%d: %s\n", __LINE__, #x);                                                            \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (false)
namespace mock
{
struct Space
{
    bool live{};
    u32 generation{};
    hw::MappingView pages;
};
std::array<Space, hw::max_address_spaces> spaces;
bool fail_create{};
uptr expect_clear{};
struct Event { hw::AddressSpace space; uptr address; usize size; u64 value; };
std::vector<Event> events;
bool observe{};
uptr deny_write{};
hw::AddressSpace observed_old{};
std::array<uptr, 6> tracked{};
std::array<u32, 6> expected{};
bool check_old_destroy{};
usize checked_old_destroys{};
usize entries{};
const hw::PageMapping *page(const hw::AddressSpace &space, uptr address)
{
    if (!space.root || space.slot >= spaces.size() || !spaces[space.slot].live ||
        spaces[space.slot].generation != space.generation)
        return nullptr;
    for (const auto &p : spaces[space.slot].pages)
        if (p.flags && p.virtual_address == (address & ~uptr(hw::page_size - 1)))
            return &p;
    return nullptr;
}
usize frames()
{
    return native::page_pool.user_count();
}
usize live()
{
    usize n = 0;
    for (const auto &s : spaces)
        n += s.live;
    return n;
}
} // namespace mock
namespace ok::arch
{
Status initialize_clock()
{
    return Status::success();
}
Result<u64> monotonic_nanoseconds()
{
    return u64{3000000042};
}
u64 clock_resolution_ns()
{
    return 1;
}
} // namespace ok::arch
namespace ok::arch::execution
{
bool supported() noexcept
{
    return false;
}
Status initialize()
{
    return Status::unsupported();
}
Result<Trap> enter_owned(AddressSpace &space, UserFrame &frame, const KernelEntryStackView &view)
{
    if (!view.valid()) return Status::invalid_argument("hosted entry requires a live lease");
    auto result = enter(space, frame);
    if (result) result.value().kernel_stack_pointer = view.lower(); // Synthetic hosted capture only.
    return result;
}
Result<Trap> enter(AddressSpace &, UserFrame &)
{
    ++mock::entries;
    return Status::unsupported();
}
Syscall syscall(const UserFrame &)
{
    return {};
}
UserFrame initial_frame(uptr pc, uptr sp)
{
    UserFrame f{};
    f.pc = pc;
    f.sp = sp;
    return f;
}
void set_return_value(UserFrame &f, i64 value)
{
    f.registers[0] = static_cast<uptr>(value);
}
Result<AddressSpace> create_space(MappingView pages)
{
    if (mock::fail_create)
        return Status::no_memory();
    for (usize i = 0; i < mock::spaces.size(); ++i)
    {
        auto &s = mock::spaces[i];
        if (s.live)
            continue;
        s.pages = pages;
        s.live = true;
        ++s.generation;
        return AddressSpace{i + 1, static_cast<u32>(i), s.generation};
    }
    return Status::no_memory();
}
bool user_range(const AddressSpace &space, uptr address, usize size, bool write)
{
    if (!size)
        return true;
    if (address < hw::user_begin || address >= hw::user_end || size > hw::user_end - address)
        return false;
    if (write && mock::deny_write && address <= mock::deny_write && mock::deny_write-address < size)
        return false;
    while (size)
    {
        auto *p = mock::page(space, address);
        if (!p || !(p->flags & (write ? page_write : page_read)))
            return false;
        usize n = hw::page_size - (address % hw::page_size);
        if (n > size)
            n = size;
        address += n;
        size -= n;
    }
    return true;
}
Status copy_from_user(const AddressSpace &space, void *out, uptr address, usize size)
{
    if (!user_range(space, address, size, false))
        return Status::fault();
    auto *dst = static_cast<std::byte *>(out);
    while (size)
    {
        auto *p = mock::page(space, address);
        usize off = address % hw::page_size, n = hw::page_size - off;
        if (n > size)
            n = size;
        std::memcpy(dst, reinterpret_cast<void *>(p->physical_address + off), n);
        dst += n;
        address += n;
        size -= n;
    }
    return Status::success();
}
Status copy_to_user(const AddressSpace &space, uptr address, const void *in, usize size)
{
    if (!user_range(space, address, size, true))
        return Status::fault();
    if (mock::observe) {
        u64 v{}; std::memcpy(&v,in,size < sizeof(v) ? size : sizeof(v));
        mock::events.push_back({space,address,size,v});
    }
    const auto *src = static_cast<const std::byte *>(in);
    while (size)
    {
        auto *p = mock::page(space, address);
        usize off = address % hw::page_size, n = hw::page_size - off;
        if (n > size)
            n = size;
        std::memcpy(reinterpret_cast<void *>(p->physical_address + off), src, n);
        src += n;
        address += n;
        size -= n;
    }
    return Status::success();
}
Status destroy_space(AddressSpace &space)
{
    CHECK(space.root && mock::spaces[space.slot].live);
    if (mock::check_old_destroy && native::same_space(space,mock::observed_old)) {
        for (usize i=0;i<mock::tracked.size();++i) {
            u32 v{};CHECK(copy_from_user(space,&v,mock::tracked[i],sizeof(v)));CHECK(v==mock::expected[i]);
        }
        ++mock::checked_old_destroys;
    }
    if (mock::expect_clear)
    {
        u32 v = 1;
        CHECK(copy_from_user(space, &v, mock::expect_clear, sizeof(v)));
        CHECK(v == 0);
        mock::expect_clear = 0;
    }
    mock::spaces[space.slot].live = false;
    mock::spaces[space.slot].pages = {};
    space = {};
    return Status::success();
}
} // namespace ok::arch::execution
std::vector<std::byte> read_file(const char *path)
{
    auto *f = std::fopen(path, "rb");
    CHECK(f);
    CHECK(std::fseek(f, 0, SEEK_END) == 0);
    auto n = std::ftell(f);
    CHECK(n > 0);
    CHECK(std::fseek(f, 0, SEEK_SET) == 0);
    std::vector<std::byte> b(static_cast<usize>(n));
    CHECK(std::fread(b.data(), 1, b.size(), f) == b.size());
    std::fclose(f);
    return b;
}
uptr word(const hw::AddressSpace &space, uptr address)
{
    uptr v{};
    CHECK(hw::copy_from_user(space, &v, address, sizeof(v)));
    return v;
}
class FaultingExecutable final : public fs::Node
{
public:
    fs::Node *backing{}, *child{};
    bool directory{}, fail_read{}, fail_release{};
    mutable usize queries{};
    usize fail_query{}, pins{}, releases{};
    std::string_view name() const override { return "fault-executable"; }
    fs::Metadata metadata() const override
    {
        return directory ? fs::Metadata{.type=fs::NodeType::directory,.mode=fs::mode_for(fs::NodeType::directory,0755)}
                         : backing->metadata();
    }
    Result<fs::Metadata> query_metadata() const override
    {
        if (++queries == fail_query) return Status::io_error("exec metadata failure");
        return metadata();
    }
    Result<fs::FileBuffer> read(usize offset, usize count) const override
    {
        return fail_read ? Result<fs::FileBuffer>{Status::denied("exec read failure")} : backing->read(offset,count);
    }
    fs::Node *lookup(std::string_view name) override { return name=="image" ? child : nullptr; }
    Status retain() override { ++pins; return Status::success(); }
    Status release() override
    {
        CHECK(pins); --pins; ++releases;
        return fail_release ? Status::io_error("exec cleanup failure") : Status::success();
    }
    Status write(usize,std::span<const std::byte>) override { return Status::unsupported(); }
    Status create(std::string_view,fs::NodeType) override { return Status::unsupported(); }
    Status chmod(u32) override { return Status::unsupported(); }
    Status chown(u32,u32) override { return Status::unsupported(); }
};
class CorruptOnClose final : public fs::Node
{
public:
    usize canary_offset{}, releases{}, pins{};
    std::string_view name() const override { return "exec-stack-corruptor"; }
    fs::Metadata metadata() const override { return {.type=fs::NodeType::regular,.mode=fs::mode_for(fs::NodeType::regular,0600)}; }
    fs::Node *lookup(std::string_view) override { return nullptr; }
    Result<fs::FileBuffer> read(usize,usize) const override { return Status::unsupported(); }
    Status write(usize,std::span<const std::byte>) override { return Status::unsupported(); }
    Status create(std::string_view,fs::NodeType) override { return Status::unsupported(); }
    Status chmod(u32) override { return Status::unsupported(); }
    Status chown(u32,u32) override { return Status::unsupported(); }
    Status retain() override { ++pins;return Status::success(); }
    Status release() override {
        CHECK(pins==1);--pins;++releases;
        native::entry_stack_arena[canary_offset]^=std::byte{1};
        return Status::success();
    }
};

void store(const hw::AddressSpace &space,uptr address,u64 value,usize size=8)
{
    CHECK(hw::copy_to_user(space,address,&value,size));
}
u32 read32(const hw::AddressSpace &space,uptr address)
{
    u32 value{};CHECK(hw::copy_from_user(space,&value,address,sizeof(value)));return value;
}
void put_list(const hw::AddressSpace &space,uptr head,u32 owner)
{
    const auto node=head+32;
    store(space,head,node);store(space,head+8,16);store(space,head+16,0);
    store(space,node,head);store(space,node+16,owner|0x80000000U,4);
}
i64 query(Kernel &kernel,sched::ProcessId pid,u64 target,uptr head,uptr length)
{
    return user::linux_bootstrap::robust_registration<abi::linux_user::Profile::x86_64>(kernel,pid,false,{target,head,length});
}
i64 registration(Kernel &kernel,sched::ProcessId pid,uptr head,usize length=24)
{
    return user::linux_bootstrap::robust_registration<abi::linux_user::Profile::x86_64>(kernel,pid,true,{head,length});
}
void lookup_cases(Kernel &kernel,sched::ProcessId pid,uptr scratch)
{
    auto *p=kernel.scheduler().find(pid);const auto space=p->resources().native_space;
    const auto leader=pid;
    auto created=kernel.scheduler().create_thread_prepared(pid,p->threads()[0].context,[](auto &thread){
        thread.native_active=true;return native::prepare_native_thread(thread);});CHECK(created);
    const auto peer=created.value();
    CHECK(kernel.scheduler().activate_thread(pid,peer));
    CHECK(registration(kernel,pid,1)==0);CHECK(registration(kernel,pid,9,23)==-22);
    CHECK(kernel.scheduler().find_thread(pid,peer)->tls.robust_list==1);
    CHECK(query(kernel,pid,0,scratch,scratch+8)==0 && word(space,scratch)==1 && word(space,scratch+8)==24);
    CHECK(query(kernel,pid,u64{1}<<32|peer,scratch,scratch+8)==0 && word(space,scratch)==1);
    CHECK(query(kernel,pid,peer,scratch,scratch)==0 && word(space,scratch)==1);
    // Partial overlap: the second native-word output overwrites the high half
    // of the length and the next four bytes, exactly in syscall order.
    store(space,scratch,~u64{});store(space,scratch+8,~u64{});
    CHECK(query(kernel,pid,peer,scratch+4,scratch)==0);
    CHECK(read32(space,scratch)==24 && word(space,scratch+4)==1);
    store(space,scratch,99);store(space,scratch+8,77);mock::deny_write=scratch;
    CHECK(query(kernel,pid,peer,scratch,scratch+8)==-14);
    CHECK(word(space,scratch)==99 && word(space,scratch+8)==24);
    store(space,scratch+8,88);
    CHECK(query(kernel,pid,peer,scratch+8,scratch)==-14 && word(space,scratch+8)==88);
    const auto before=mock::events.size();
    CHECK(query(kernel,pid,0xffffffffU,scratch,scratch)==-3);
    CHECK(query(kernel,pid,999999,scratch,scratch)==-3);
    auto outside=kernel.scheduler().create_user_process("outside",p->threads()[0].context);CHECK(outside);
    kernel.scheduler().find(outside.value())->resources().personality=sched::UserPersonality::linux;
    CHECK(query(kernel,pid,outside.value(),scratch,scratch)==-1);
    CHECK(mock::events.size()==before);mock::deny_write=0;
    // A Linux-exited leader remains a target until group reaping, with its
    // registration cleared; exited peer slots must become invisible.
    CHECK(kernel.scheduler().activate_thread(pid,leader));CHECK(registration(kernel,pid,3)==0);
    bool complete=true;CHECK(native::dispatch(kernel,pid,{OK_SYS_EXIT,{0}},complete)==0 && !complete);
    CHECK(kernel.scheduler().activate_thread(pid,peer));
    CHECK(query(kernel,pid,leader,scratch,scratch+8)==0 && word(space,scratch)==0);
    auto other=kernel.scheduler().create_thread_prepared(pid,p->threads()[0].context,[](auto &thread){
        thread.native_active=true;return native::prepare_native_thread(thread);});CHECK(other);
    CHECK(kernel.scheduler().activate_thread(pid,other.value()));
    complete=true;CHECK(native::dispatch(kernel,pid,{OK_SYS_EXIT,{0}},complete)==0 && !complete);
    CHECK(kernel.scheduler().activate_thread(pid,peer));
    CHECK(query(kernel,pid,other.value(),scratch,scratch+8)==-3);
    // Fork resets registration while the original value remains unchanged.
    auto child=kernel.scheduler().fork_process(pid,false);CHECK(child);
    CHECK(kernel.scheduler().find(child.value())->threads()[0].tls.robust_list==0);
    CHECK(kernel.scheduler().find(child.value())->threads()[0].tls.robust_list_length==0);
    CHECK(kernel.scheduler().find_thread(pid,peer)->tls.robust_list==1);
    std::puts("PASS SDK28 lookup: caller/peer/signed32/overlap/fault-order/cross-group/leader-zombie/retired-peer/fork-reset guest=0");
}
int main(int argc,char **argv)
{
    CHECK(argc==3);const bool fatal=std::strcmp(argv[2],"fatal-new")==0 || std::strcmp(argv[2],"fatal-old")==0;
    const bool oldowner=std::strstr(argv[2],"old")!=nullptr;
    static Kernel kernel;CHECK(kernel.boot({}));CHECK(kernel.posix().activate(0));
    CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    auto bytes=read_file(argv[1]);bytes[7]=std::byte{3};
    CHECK(kernel.vfs().create_readonly_file("/old",bytes));CHECK(kernel.vfs().create_readonly_file("/target",bytes));
    native::Arguments args;CHECK(args.argv.push_back(FixedString<256>("old")));
    auto image=native::load(kernel,"/old",args);CHECK(image);
    auto created=kernel.scheduler().create_user_process("sdk28-mask-regression",kernel.arch().make_user_context({image.value().frame.pc,image.value().frame.sp}));CHECK(created);
    const auto pid=created.value();auto *process=kernel.scheduler().find(pid);
    CHECK(native::prepare_native_thread(process->threads()[0]));native::install(*process,image.value());
    CHECK(process->resources().personality==sched::UserPersonality::linux);
    CHECK(process->threads()[0].native_signal_mask==sched::SignalMask{});
    CHECK(kernel.scheduler().set_runnable(pid));CHECK(kernel.posix().activate(pid));
    const auto old_space=process->resources().native_space;
    const uptr scratch=process->threads()[0].native_frame.sp-4096;
    if(std::strcmp(argv[2],"lookup")==0) {mock::observe=true;lookup_cases(kernel,pid,scratch);return 0;}
    auto first=kernel.scheduler().create_thread_prepared(pid,process->threads()[0].context,[](auto &t){t.native_active=true;return native::prepare_native_thread(t);});CHECK(first);
    auto second=kernel.scheduler().create_thread_prepared(pid,process->threads()[0].context,[](auto &t){t.native_active=true;return native::prepare_native_thread(t);});CHECK(second);
    const auto caller=first.value(), sibling=second.value();
    sched::SignalMask leader_mask, caller_mask, sibling_mask;
    leader_mask.set(10); caller_mask.set(12); sibling_mask.set(14);
    process->threads()[0].native_signal_mask=leader_mask;
    kernel.scheduler().find_thread(pid,caller)->native_signal_mask=caller_mask;
    kernel.scheduler().find_thread(pid,sibling)->native_signal_mask=sibling_mask;
    const auto damaged=kernel.scheduler().find_thread(pid,caller)->entry_stack;
    const auto later=kernel.scheduler().find_thread(pid,sibling)->entry_stack;
    std::vector<uptr> old_pages;
    for (const auto &page : process->resources().native_pages) old_pages.push_back(page.physical_address);
    const auto old_frames=mock::frames();
    const uptr head0=scratch+256,head1=scratch+512,head2=scratch+768;
    const uptr clear0=scratch+1024,clear1=scratch+1032,clear2=scratch+1040;
    put_list(old_space,head0,pid);put_list(old_space,head1,oldowner?caller:pid);put_list(old_space,head2,sibling);
    for(auto [tid,head,clear]: {std::tuple{pid,head0,clear0},std::tuple{caller,head1,clear1},std::tuple{sibling,head2,clear2}}) {
        CHECK(kernel.scheduler().activate_thread(pid,tid));CHECK(registration(kernel,pid,head)==0);
        kernel.scheduler().current_thread()->tls.clear_tid_address=clear;store(old_space,clear,tid,4);
    }
    CHECK(kernel.scheduler().activate_thread(pid,caller));
    const char path[]="/target";CHECK(hw::copy_to_user(old_space,scratch,path,sizeof(path)));
    mock::observed_old=old_space;
    mock::tracked={head0+48,head1+48,head2+48,clear0,clear1,clear2};
    mock::expected={0xc0000000U,oldowner?static_cast<u32>(caller|0x80000000U):0xc0000000U,0xc0000000U,0,static_cast<u32>(caller),0};
    mock::observe=true;bool complete=true;
    // The actual dispatcher must leave registrations and owner words intact on
    // both image-allocation and entry-stack-validation failures.
    for(bool loader_failure:{true,false}) {
        mock::events.clear();mock::fail_create=loader_failure;
        if(!loader_failure) kernel.scheduler().find_thread(pid,caller)->kernel_stack_top^=1;
        const auto result=native::dispatch(kernel,pid,{OK_SYS_EXECVE,{scratch,0,0,0,0,0}},complete);
        CHECK(result<0 && complete);mock::fail_create=false;
        if(!loader_failure) kernel.scheduler().find_thread(pid,caller)->kernel_stack_top^=1;
        CHECK(process->threads().size()==3 && mock::frames()==old_frames && mock::live()==1);
        CHECK(process->threads()[0].native_signal_mask==leader_mask);
        CHECK(kernel.scheduler().find_thread(pid,caller)->native_signal_mask==caller_mask);
        CHECK(kernel.scheduler().find_thread(pid,sibling)->native_signal_mask==sibling_mask);
        CHECK(native::same_space(old_space,process->resources().native_space));
        CHECK(kernel.scheduler().find_thread(pid,caller)->tls.robust_list==head1);
        CHECK(process->threads()[0].tls.robust_list==head0);
        CHECK(kernel.scheduler().find_thread(pid,sibling)->tls.robust_list==head2);
        CHECK(read32(old_space,head0+48)==(pid|0x80000000U));
        CHECK(read32(old_space,head1+48)==((oldowner?caller:pid)|0x80000000U));
        for(const auto &e:mock::events) if(native::same_space(e.space,old_space))
            for(auto tracked:mock::tracked) CHECK(e.address!=tracked);
    }
    CorruptOnClose fault;
    if(fatal) {
        fault.canary_offset=damaged.slot*memory::KernelEntryStackPool::slot_stride;
        auto fd=kernel.scheduler().allocate_description(sched::OpenFileDescription{.used=true,.readable=true,.node=&fault});CHECK(fd);
        CHECK(kernel.scheduler().install_descriptor(process->resources(),7,fd.value(),true));
    }
    mock::events.clear();mock::check_old_destroy=true;native::started=true;native::runtime_kernel=&kernel;
    CHECK(native::dispatch(kernel,pid,{OK_SYS_EXECVE,{scratch,0,0,0,0,0}},complete)==0 && !complete);
    CHECK(process->threads().size()==1 && process->threads()[0].tls.robust_list==0);
    std::vector<uptr> order;
    for(const auto &e:mock::events) for(auto tracked:mock::tracked) if(e.address==tracked) {
        CHECK(native::same_space(e.space,old_space));order.push_back(e.address);
    }
    std::vector<uptr> expected_order{head0+48,clear0,head2+48,clear2};
    if(!oldowner) expected_order.push_back(head1+48);
    CHECK(order==expected_order);
    if(fatal) {
        CHECK(!native::started && !native::runtime_status() && mock::checked_old_destroys==0);
        CHECK(native::same_space(old_space,process->resources().native_space));
        for(usize i=0;i<mock::tracked.size();++i) CHECK(read32(old_space,mock::tracked[i])==mock::expected[i]);
        CHECK(fault.releases==1 && fault.pins==0);
        CHECK(native::stranded_entry_stacks[damaged.slot].handle==damaged);
        CHECK(native::stranded_entry_stacks[later.slot].handle==later);
        usize held_count=0,held_pages=0;
        for(const auto &held:native::quarantined_spaces) if(held.space.root) {
            ++held_count;held_pages+=held.pages.size();CHECK(!native::same_space(held.space,old_space));
        }
        CHECK(held_count==1 && held_pages>0 && mock::live()==2 && mock::frames()==old_frames+held_pages);
        for(const auto address:old_pages) CHECK(native::page_pool.owns(address));
        const auto events=mock::events.size(),entries=mock::entries;
        CHECK(!native::step(kernel) && mock::events.size()==events && mock::entries==entries);
    } else {
        CHECK(native::started && native::runtime_status() && mock::checked_old_destroys==1 && mock::live()==1);
        CHECK(process->threads()[0].native_signal_mask==caller_mask);
        CHECK(!native::same_space(old_space,process->resources().native_space));
        CHECK(native::entry_stacks.accounting().leased_slots==1 && native::entry_stacks.accounting().quarantined_slots==0);
        // The replacement maps the same user-stack addresses but its contents
        // were never targets of retirement writes.
        CHECK(kernel.scheduler().activate_thread(pid,pid));
        CHECK(query(kernel,pid,0,scratch,scratch+8)==0 && word(process->resources().native_space,scratch)==0);
        auto &frame=process->threads()[0].native_frame;
        frame.registers[14]=*lu::Codec<lu::Profile::x86_64>::syscall_number(lu::Operation::rt_sigprocmask);
        frame.registers[9]=999;frame.registers[10]=0;frame.registers[11]=scratch;frame.registers[5]=8;
        bool done=true;
        CHECK(user::linux_bootstrap::dispatch<lu::Profile::x86_64>(kernel,pid,frame,done,
            [&](hw::Syscall call,bool &completed){return native::dispatch(kernel,pid,call,completed);})==0 && done);
        CHECK(word(process->resources().native_space,scratch)==caller_mask.words[0]);
    }
    std::printf("PASS SDK28 actual runtime exec mode=%s: precommit preservation; sibling old IDs; caller process ID; robust-before-clear; old VM only; committed terminal/normal path guest=0 smp=0\n",argv[2]);
}
