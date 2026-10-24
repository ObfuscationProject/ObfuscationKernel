#include "ok/user/robust_futex.hpp"
#include <cstdio>
#include <cstdlib>
using namespace ok;
using namespace ok::user::native;
namespace lu=ok::abi::linux_user;
static usize checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"%d: %s\n",__LINE__,#x); std::abort(); } } while (false)
template <lu::Profile P> struct Memory
{
    static constexpr bool serialized_native_execution=true;
    static constexpr usize word=RobustListCodec<P>::word_bytes;
    static constexpr bool be=RobustListCodec<P>::big_endian;
    std::array<std::byte,8192> bytes{};
    u64 denied_read=~u64{0},denied_write=~u64{0};
    usize reads{},compares{},wakes{};
    u64 last_wake{};
    bool race{};
    void put(u64 address,u64 value,usize size=word)
    { CHECK(address<=bytes.size() && size<=bytes.size()-address);for(usize i=0;i<size;++i)bytes[address+i]=std::byte(value>>(8*(be?size-1-i:i))); }
    u64 get(u64 address,usize size=word)
    { u64 value{};for(usize i=0;i<size;++i)value|=static_cast<u64>(bytes[address+i])<<(8*(be?size-1-i:i));return value; }
    Status read(u64 address,std::span<std::byte> out)
    {
        ++reads;
        if(address>bytes.size() || out.size()>bytes.size()-address ||
           (address<=denied_read && denied_read-address<out.size()))return Status::fault();
        for(usize i=0;i<out.size();++i)out[i]=bytes[address+i];
        return Status::success();
    }
    Result<u32> compare_exchange32(u64 address,u32 expected,u32 value)
    {
        ++compares;
        if(address==denied_write)return Status::fault();
        if(race){race=false;put(address,98,4);return u32{98};}
        const auto old=static_cast<u32>(get(address,4));if(old==expected)put(address,value,4);return old;
    }
    void wake_one(u64 address){++wakes;last_wake=address;}
    void head(u64 next=64,u64 offset=16,u64 pending=0)
    {put(64,next);put(64+word,offset);put(64+2*word,pending);}
};
template <lu::Profile P> void profile()
{
    using M=Memory<P>;using C=RobustListCodec<P>;
    CHECK(C::head_bytes==3*C::word_bytes);
    CHECK(C::add(C::mask,2)==1);
    {M m;auto r=retire_robust_list<P>(0,17,m);CHECK(r.stop==RobustStop::complete && !m.reads);}
    {M m;m.head();auto r=retire_robust_list<P>(64,17,m);CHECK(r.stop==RobustStop::complete && !r.visited && m.reads==3);}
    for(u32 flags:{u32{0},robust_waiters,robust_owner_died,robust_waiters|robust_owner_died})
    {
        M m;m.head(256);m.put(256,64);m.put(272,17|flags,4);
        auto r=retire_robust_list<P>(64,17,m);
        CHECK(r.stop==RobustStop::complete && r.visited==1 && r.marked==1);
        CHECK(m.get(272,4)==((flags&robust_waiters)|robust_owner_died));
        CHECK(m.wakes==static_cast<usize>(bool(flags&robust_waiters)) && m.compares==1);
    }
    {M m;m.head(256);m.put(256,64);m.put(272,99|robust_waiters,4);auto r=retire_robust_list<P>(64,17,m);CHECK(r.marked==0 && !m.wakes && !m.compares && m.get(272,4)==(99|robust_waiters));}
    for(u32 flags:{u32{0},robust_waiters,robust_owner_died,robust_waiters|robust_owner_died})
    {
        M m;m.head(64,16,256);m.put(272,flags,4);auto r=retire_robust_list<P>(64,17,m);
        CHECK(r.marked==0 && r.wake_attempts==1 && m.wakes==1 && !m.compares && m.get(272,4)==flags);
    }
    {M m;m.head(256,16,256);m.put(256,64);m.put(272,17|robust_waiters,4);auto r=retire_robust_list<P>(64,17,m);CHECK(r.marked==1 && m.wakes==1 && m.compares==1);}
    {M m;m.head(257);m.put(256,64);m.put(272,17|robust_waiters,4);auto r=retire_robust_list<P>(64,17,m);CHECK(r.marked==1 && r.pi_entries==1 && !m.wakes);}
    {M m;m.head(64,16,257);m.put(272,0,4);auto r=retire_robust_list<P>(64,17,m);CHECK(r.marked==0 && r.pi_entries==1 && !m.wakes);}
    {M m;m.head(256,C::mask-7);m.put(256,64);m.put(248,17,4);auto r=retire_robust_list<P>(64,17,m);CHECK(r.marked==1 && m.get(248,4)==robust_owner_died);}
    for(usize field=0;field<3;++field)
    {M m;m.head();m.denied_read=64+field*M::word;auto r=retire_robust_list<P>(64,17,m);CHECK(r.stop==RobustStop::head_fault && m.reads==field+1 && !m.compares && !m.wakes);}
    {M m;m.head(256,16,512);m.put(272,17|robust_waiters,4);m.put(528,17,4);m.denied_read=256;auto r=retire_robust_list<P>(64,17,m);CHECK(r.stop==RobustStop::node_fault && r.marked==1 && m.get(528,4)==17 && m.wakes==1);}
    {M m;m.head(256);m.put(256,64);m.put(272,17,4);m.denied_write=272;auto r=retire_robust_list<P>(64,17,m);CHECK(r.stop==RobustStop::word_fault && m.get(272,4)==17 && !m.wakes);}
    {M m;m.head(256,17);m.put(256,64);auto r=retire_robust_list<P>(64,17,m);CHECK(r.stop==RobustStop::word_fault && !m.compares);}
    {M m;m.head(256);m.put(256,64);m.put(272,17|robust_waiters,4);m.race=true;auto r=retire_robust_list<P>(64,17,m);CHECK(r.stop==RobustStop::complete && !r.marked && m.compares==1 && m.get(272,4)==98 && !m.wakes);}
    {M m;m.head(256,16,512);m.put(256,256);m.put(272,17,4);m.put(528,17|robust_waiters,4);auto r=retire_robust_list<P>(64,17,m);CHECK(r.stop==RobustStop::limit && r.visited==2048 && r.marked==2 && m.wakes==1 && m.compares==2);}
    // The current lock is handled before rejecting an inaccessible next link.
    // A failed current-owner copy, however, terminates before pending handling.
    {M m;m.head(256,16,512);m.put(256,64);m.put(528,17,4);m.denied_read=272;auto r=retire_robust_list<P>(64,17,m);CHECK(r.stop==RobustStop::word_fault && m.get(528,4)==17 && !m.compares);}
}
int main()
{
    profile<lu::Profile::i386>();profile<lu::Profile::x86_64>();profile<lu::Profile::aarch64>();
    profile<lu::Profile::arm32>();profile<lu::Profile::rv64>();profile<lu::Profile::rv32>();
    profile<lu::Profile::loongarch64>();profile<lu::Profile::mips>();profile<lu::Profile::mips64>();profile<lu::Profile::ppc>();
    std::printf("PASS robust-futex checks=%zu scope=hosted-model guest=0 smp=0\n",checks);
}
