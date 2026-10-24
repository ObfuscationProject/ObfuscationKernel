#include "ok/user/native_signal_action.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace ok;
using namespace ok::user::native;
using Profile=abi::linux_user::Profile;
using Action=sched::SignalAction;
using Table=sched::SignalActionTable;
static usize checks{};
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"%d: %s\n",__LINE__,#x);std::abort();}}while(0)
struct Layout {usize word,size,handler,flags,restorer,mask,flag_bytes;bool big;u64 uapi;u32 stop,maximum,child,cont,winch,urg;};
// Values independently transcribed from retained Linux 6.13.4 definitions and
// the prior extracted-struct cross-compile oracle. No candidate metadata is
// used to choose expected widths, offsets, endian or flag masks.
constexpr Layout layouts[]{
 {4,20,0,4,8,12,4,false,0xdc000807,19,64,17,18,28,23},
 {8,32,0,8,16,24,8,false,0xdc000807,19,64,17,18,28,23},
 {4,20,0,4,8,12,4,false,0xde000807,19,64,17,18,28,23},
 {8,32,0,8,16,24,8,false,0xdc000807,19,64,17,18,28,23},
 {4,16,0,4,999,8,4,false,0xd8000807,19,64,17,18,28,23},
 {8,24,0,8,999,16,8,false,0xd8000807,19,64,17,18,28,23},
 {8,24,0,8,999,16,8,false,0xd8000807,19,64,17,18,28,23},
 {4,24,4,0,999,8,4,true,0xd8010809,23,128,18,25,20,21},
 {8,32,8,0,999,16,4,true,0xd8010809,23,128,18,25,20,21},
 {4,20,0,4,8,12,4,true,0xdc000807,19,64,17,18,28,23}
};
static u64 rng=0x442880665f17195bULL;
u64 random_word(){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;}
std::array<std::byte,32> pack(const Layout &l,Action a)
{
    std::array<std::byte,32> bytes{};
    const auto scalar=[&](usize at,usize count,u64 v){for(usize b=0;b<count;++b)bytes[at+b]=std::byte(v>>(8*(l.big?count-b-1:b)));};
    scalar(l.handler,l.word,a.handler);scalar(l.flags,l.flag_bytes,a.flags);
    if(l.restorer!=999)scalar(l.restorer,l.word,a.restorer);
    for(usize bit=0;bit<l.maximum;++bit) if(a.mask.words[bit/64]&(u64{1}<<(bit%64))) {
        const auto word=bit/(8*l.word),position=bit%(8*l.word);
        const auto at=l.mask+word*l.word+(l.big?l.word-1-position/8:position/8);
        bytes[at]|=std::byte(1U<<(position%8));
    }
    return bytes;
}
Action sanitize(const Layout &l,Action a)
{
    if(l.word==4) {a.handler&=0xffffffffU;a.restorer&=0xffffffffU;}
    if(l.restorer==999)a.restorer=0;
    a.flags&=l.uapi;a.mask.words[0]&=~((u64{1}<<8)|(u64{1}<<(l.stop-1)));
    if(l.maximum==64)a.mask.words[1]=0;
    return a;
}
bool ignored(const Layout &l,u32 sig,Action a) {return a.handler==1 || (a.handler==0 && (sig==l.child||sig==l.cont||sig==l.winch||sig==l.urg));}
struct Memory {
    std::array<std::byte,128> bytes{};
    std::array<char,3> events{};usize count{},read_limit{32},write_limit{32};
    usize read_size{},write_size{};u32 discarded{};
    bool read(u64 address,std::span<std::byte> out) {
        events[count++]='R';read_size=out.size();if(address>=bytes.size()||out.size()>bytes.size()-address)return false;
        const auto n=std::min(read_limit,out.size());std::copy_n(bytes.begin()+address,n,out.begin());return n==out.size();
    }
    bool write(u64 address,std::span<const std::byte> in) {
        events[count++]='W';write_size=in.size();if(address>=bytes.size()||in.size()>bytes.size()-address)return false;
        const auto n=std::min(write_limit,in.size());std::copy_n(in.begin(),n,bytes.begin()+address);return n==in.size();
    }
};
template<Profile P> void profile_test(const char *name,const Layout &l)
{
    using C=SignalActionCodec<P>;
    CHECK(C::word_bytes==l.word&&C::bytes==l.size&&C::handler_offset==l.handler&&C::flags_offset==l.flags&&C::mask_offset==l.mask&&C::flags_bytes==l.flag_bytes);
    CHECK((C::has_restorer?C::restorer_offset:999)==l.restorer&&C::big_endian==l.big&&C::uapi_flags==l.uapi&&C::stop_signal==l.stop&&C::max_signal==l.maximum);
    std::printf("LAYOUT %s %zu %zu %zu %zu %zu %zu\n",name,C::bytes,C::handler_offset,C::flags_offset,C::has_restorer?C::restorer_offset:999,C::mask_offset,C::flags_bytes);
    Action raw{0x0123456789abcdefULL,0x8877665544332211ULL,0xfedcba9876543210ULL,{{0x8000000180440181ULL,0x8000000180000001ULL}}};
    auto literal=pack(l,raw);std::array<std::byte,C::bytes> input{};std::copy_n(literal.begin(),l.size,input.begin());
    if constexpr(P==Profile::mips64)for(usize i=4;i<8;++i)input[i]=std::byte{0xef};
    auto decoded=C::decode(input);Action expected=raw;
    if(l.word==4){expected.handler&=0xffffffffU;expected.restorer&=0xffffffffU;}
    if(l.flag_bytes==4)expected.flags&=0xffffffffU;
    if(l.restorer==999)expected.restorer=0;
    if(l.maximum==64)expected.mask.words[1]=0;
    CHECK(decoded==expected);auto encoded=C::encode(decoded);CHECK(std::equal(encoded.begin(),encoded.end(),literal.begin()));
    // Every flag bit and every mask bit is checked against source-defined
    // sanitation, including native high bits, ARM32 THIRTYTWO, MIPS SIGINFO,
    // EXPOSE_TAGBITS and the deliberately unsupported probing bit.
    for(u32 bit=0;bit<64;++bit) {Action a{};a.flags=u64{1}<<bit;CHECK(C::sanitize(a)==sanitize(l,a));}
    for(u32 bit=0;bit<128;++bit){Action a{};a.mask.words[bit/64]=u64{1}<<(bit%64);CHECK(C::sanitize(a)==sanitize(l,a));}
    for(u32 sig=1;sig<=128;++sig)CHECK(C::default_ignored(sig)==(sig==l.child||sig==l.cont||sig==l.winch||sig==l.urg));
    Table table;
    const auto invoke=[&](Memory &mem,i32 signal,u64 in,u64 out,u64 size) {
        return update_signal_action<P>(table,signal,in,out,size,
            [&](u64 a,auto span){return mem.read(a,span);},[&](u64 a,auto span){return mem.write(a,span);},
            [&](u32 s) noexcept {CHECK(ignored(l,s,*table.find(s)));mem.events[mem.count++]='D';mem.discarded=s;});
    };
    // Query-only KILL/STOP succeeds; setting either is rejected after reading.
    for(u32 sig:{9U,l.stop}) {
        Memory mem;CHECK(invoke(mem,sig,0,64,C::Mask::bytes)==0&&mem.count==1&&mem.events[0]=='W');
        mem={};CHECK(invoke(mem,sig,16,64,C::Mask::bytes)==-22&&mem.count==1&&mem.events[0]=='R');
    }
    for(i32 sig:{-1,0,static_cast<i32>(l.maximum+1)}) {
        Memory mem;CHECK(invoke(mem,sig,~u64{0},64,0)==-22&&mem.count==0);
        CHECK(invoke(mem,sig,~u64{0},64,C::Mask::bytes)==-14&&mem.count==1);
        mem={};CHECK(invoke(mem,sig,16,64,C::Mask::bytes)==-22&&mem.count==1);
    }
    // 1,200 deterministic hostile updates with an independently computed
    // expected state, exact output memory and exact R / discard / W order.
    for(usize iteration=0;iteration<1200;++iteration) {
        const u32 signal=iteration%17==0?l.child:iteration%19==0?l.cont:iteration%23==0?l.winch:iteration%29==0?l.urg:12;
        Action old=sanitize(l,{random_word(),random_word(),random_word(),{{random_word(),random_word()}}});
        if(iteration%7==0)old.handler=1;
        *table.find(signal)=old;
        Action desired{random_word(),random_word(),random_word(),{{random_word(),random_word()}}};
        if(iteration%3==0)desired.handler=0;else if(iteration%3==1)desired.handler=1;
        const auto in=iteration%11==0?0U:9U;
        const auto out=iteration%13==0?0U:iteration%3==0?9U:iteration%3==1?12U:64U;
        const auto size=iteration%31==0?C::Mask::bytes+1:C::Mask::bytes;
        Memory mem;mem.bytes.fill(std::byte{0xa5});const auto data=pack(l,desired);
        if(in)std::copy_n(data.begin(),l.size,mem.bytes.begin()+in);
        if(iteration%5==0)mem.read_limit=l.size/2;
        if(iteration%7==0)mem.write_limit=l.size/2;
        auto expected_memory=mem.bytes;Action wanted=old;std::array<char,3> order{};usize count{};u32 discarded{};i64 status{};
        if(size!=C::Mask::bytes)status=-22;
        else {
            if(in) {
                order[count++]='R';
                if(mem.read_limit<l.size)status=-14;
                else {wanted=sanitize(l,desired);if(ignored(l,signal,wanted)){order[count++]='D';discarded=signal;}}
            }
            if(status==0&&out) {order[count++]='W';const auto before=pack(l,old);std::copy_n(before.begin(),std::min(l.size,mem.write_limit),expected_memory.begin()+out);if(mem.write_limit<l.size)status=-14;}
        }
        CHECK(invoke(mem,signal,in,out,size)==status);CHECK(*table.find(signal)==wanted);
        CHECK(mem.bytes==expected_memory&&mem.events==order&&mem.count==count&&mem.discarded==discarded);
        if(mem.read_size)CHECK(mem.read_size==l.size);
        if(mem.write_size)CHECK(mem.write_size==l.size);
    }
    // Model both shared and all eight directed pending sets. The required hook
    // runs after commit and survives output EFAULT, even for already-ignored
    // actions; unrelated pending work is untouched.
    for(u32 signal:{12U,l.child,l.cont,l.winch,l.urg}) {
        for(u64 handler:{u64{0},u64{1}}) {
            SignalMask shared;std::array<SignalMask,8> directed;
            shared.set(signal);shared.set(63);for(auto &pending:directed)pending=shared;
            Action set{};set.handler=handler;auto data=C::encode(set);Memory mem;
            std::copy(data.begin(),data.end(),mem.bytes.begin()+16);mem.write_limit=1;
            bool invoked=false;
            CHECK(update_signal_action<P>(table,signal,16,64,C::Mask::bytes,
                [&](u64 a,auto b){return mem.read(a,b);},[&](u64 a,auto b){return mem.write(a,b);},
                [&](u32 sig) noexcept {CHECK(table.find(sig)->handler==handler);invoked=true;shared.set(sig,false);for(auto &pending:directed)pending.set(sig,false);})==-14);
            const bool drop=ignored(l,signal,set);CHECK(invoked==drop&&shared.contains(signal)==!drop&&shared.contains(63));
            for(const auto &pending:directed)CHECK(pending.contains(signal)==!drop&&pending.contains(63));
        }
    }
    // Query strips internal/unknown flags from returned state but cannot mutate
    // the stored action. No pending callback is permitted for a pure query.
    Action dirty=raw;dirty.flags=~u64{0};*table.find(12)=dirty;Memory query;
    CHECK(invoke(query,12,0,64,C::Mask::bytes)==0&&query.discarded==0&&*table.find(12)==dirty);
    dirty.flags&=l.uapi;const auto safe=pack(l,dirty);CHECK(std::equal(safe.begin(),safe.begin()+l.size,query.bytes.begin()+64));
    // Raw kernel signal 32/33/34 and the architecture maximum are valid even
    // where libc public sigaction imposes additional restrictions.
    for(u32 sig:{32U,33U,34U,l.maximum}) {Memory mem;CHECK(invoke(mem,sig,16,0,C::Mask::bytes)==0);}
    std::printf("PASS actions profile=%s randomized=1200 internal_only=1\n",name);
}
void table_lifetime()
{
    Table parent;CHECK(parent.find(0)==nullptr&&parent.find(129)==nullptr);
    for(u32 sig=1;sig<=128;++sig) {
        auto *a=parent.find(sig);CHECK(a&&*a==Action{});*a={sig%3==0?1ULL:0x1000ULL+sig,0xffff,0x4000+sig,{{~u64{0},~u64{0}}}};
    }
    const auto before=parent;auto child=parent.fork_copy();CHECK(child==parent);
    child.find(128)->handler=77;CHECK(parent==before);
    // Shared same-process references see the same table; no simulated separate
    // CLONE_SIGHAND owner or scheduler ownership is being claimed.
    auto &thread_a=parent;auto &thread_b=parent;thread_a.find(64)->handler=1;CHECK(thread_b.find(64)->handler==1);
    const auto copied=parent;parent.reset_for_exec();
    for(u32 sig=1;sig<=128;++sig) {Action wanted{};if(copied.find(sig)->handler==1)wanted.handler=1;CHECK(*parent.find(sig)==wanted);}
    CHECK(child.find(128)->handler==77);parent.reset_for_exec(true);for(u32 sig=1;sig<=128;++sig)CHECK(*parent.find(sig)==Action{});
    CHECK(Table::storage_bytes==5120);
}
int main()
{
    profile_test<Profile::i386>("i386",layouts[0]);profile_test<Profile::x86_64>("x86_64",layouts[1]);
    profile_test<Profile::arm32>("arm32",layouts[2]);profile_test<Profile::aarch64>("aarch64",layouts[3]);
    profile_test<Profile::rv32>("rv32",layouts[4]);profile_test<Profile::rv64>("rv64",layouts[5]);
    profile_test<Profile::loongarch64>("loongarch64",layouts[6]);profile_test<Profile::mips>("mips",layouts[7]);
    profile_test<Profile::mips64>("mips64",layouts[8]);profile_test<Profile::ppc>("ppc",layouts[9]);
    table_lifetime();std::printf("PASS internal signal action checks=%zu profiles=10 guest=0 delivery=0\n",checks);
}
