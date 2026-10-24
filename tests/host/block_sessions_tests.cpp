#include <ok/block/sessions.hpp>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>
#include <vector>

using namespace ok;
using namespace ok::block;
using Registry = BlockSessions<2>;
namespace
{
usize checks{};
void check(bool value, const char *why)
{
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL block sessions: %s\n", why); std::exit(1); }
}
struct Device final : driver::BlockDevice
{
    driver::BlockGeometry shape{32,512,true};
    std::vector<std::byte> data = std::vector<std::byte>(32*512);
    usize reads{}, writes{}, flushes{};
    bool fail_read{}, fail_write{}, fail_flush{}, change_after{};
    u64 origin{}, last_sector{};
    std::function<void()> callback;
    std::function<void()> geometry_callback;
    mutable std::atomic<usize> geometry_calls{};
    driver::BlockGeometry geometry() const override
    {
        geometry_calls.fetch_add(1,std::memory_order_relaxed);
        if (geometry_callback) geometry_callback();
        return shape;
    }
    Status read_blocks(u64 first, std::span<std::byte> out) override
    {
        ++reads; last_sector=first;
        if (callback) callback();
        if (change_after) --shape.block_count;
        if (fail_read) return Status::io_error("injected read");
        if (first<origin || first-origin>=32 || out.size()/512>32-(first-origin)) return Status::io_error("model bounds");
        std::copy_n(data.begin()+(first-origin)*512,out.size(),out.begin()); return {};
    }
    Status write_blocks(u64 first, std::span<const std::byte> in) override
    {
        ++writes; last_sector=first;
        if (callback) callback();
        if (change_after) --shape.block_count;
        if (first<origin || first-origin>=32 || in.size()/512>32-(first-origin)) return Status::io_error("model bounds");
        std::copy(in.begin(),in.end(),data.begin()+(first-origin)*512);
        return fail_write ? Status::io_error("injected ambiguous write") : Status{};
    }
    Status flush() override
    {
        ++flushes;
        if (callback) callback();
        if (change_after) --shape.block_count;
        return fail_flush ? Status::io_error("injected ambiguous flush") : Status{};
    }
};
}
int main()
{
    static_assert(std::is_same_v<SessionReport::counter_type, u64>);
    static_assert(std::is_same_v<Registry::Session::counter_type, u64>);
    static_assert(std::is_same_v<decltype(SessionReport::reads), u64>);
    static_assert(!std::is_copy_constructible_v<Registry::Session>);
    static_assert(!std::is_move_constructible_v<Registry::Session>);
    static_assert(!std::is_copy_constructible_v<Registry>);
    std::array<std::byte,512> bytes{};
    {
        Device disk;Registry service;Registry::Session session,next;
        check(session.transfer_limit().status().code()==StatusCode::not_initialized,"unbound transfer limit");
        const auto key=service.register_device(disk,1,1024);check(key.ok(),"transfer limit register");
        check(service.acquire(key.value(),{0,32},SessionAccess::readonly,session).ok(),"transfer limit acquire");
        check(session.transfer_limit().ok()&&session.transfer_limit().value()==1024,"checked immutable transfer limit");
        disk.callback=[&]{check(session.transfer_limit().value()==1024,"in-flight transfer limit queries no transport");};
        std::array<std::byte,512> data{};check(session.read_blocks(0,data).ok(),"transfer limit read");
        check(session.close().ok()&&session.transfer_limit().status().code()==StatusCode::bad_descriptor,"closed transfer limit rejected");
        check(service.acquire(key.value(),{0,32},SessionAccess::readonly,next).ok()&&
              session.transfer_limit().status().code()==StatusCode::bad_descriptor,"stale serial transfer limit rejected");
        check(next.abort(Status::interrupted("limit test")).ok()&&next.transfer_limit().status().code()==StatusCode::denied,"quarantined transfer limit rejected");
    }

    {
        Device disk,alias;Registry service;Registry::Session session;
        const auto key=service.register_device(disk,1);check(key.ok(),"duplicate query register");
        const auto queries=disk.geometry_calls.load();
        check(!service.register_device(disk,2)&&!service.register_device(alias,1),"ready duplicate rejected");
        check(disk.geometry_calls==queries&&alias.geometry_calls==0,"ready duplicate has no raw geometry query");
        check(service.acquire(key.value(),{0,32},SessionAccess::writable,session).ok(),"duplicate query acquire");
        check(!service.register_device(disk,2)&&disk.geometry_calls==queries,"owned duplicate has no raw geometry query");
        disk.callback=[&] {
            const auto before=disk.geometry_calls.load();
            check(!service.register_device(disk,2)&&!service.register_device(alias,1),"in-flight duplicate rejected");
            check(disk.geometry_calls==before&&alias.geometry_calls==0,"in-flight duplicate has no raw geometry query");
        };
        disk.fail_read=true;
        check(!session.read_blocks(0,bytes),"duplicate query enters quarantine");
        const auto failed_queries=disk.geometry_calls.load();
        check(!service.register_device(disk,2)&&disk.geometry_calls==failed_queries,"quarantined duplicate has no raw geometry query");
    }
    {
        Device stale,disk,alias,other,full;Registry service;
        const auto old=service.register_device(stale,9);check(old.ok()&&service.detach(old.value()).ok(),"reservation stale key setup");
        disk.shape.block_count=0;
        std::atomic<bool> entered{},leave{};StatusCode result=StatusCode::ok;
        disk.geometry_callback=[&] {entered.store(true,std::memory_order_release);while(!leave.load(std::memory_order_acquire))std::this_thread::yield();};
        std::thread worker([&] {result=service.register_device(disk,1).status().code();});
        while(!entered.load(std::memory_order_acquire))std::this_thread::yield();
        check(!service.register_device(disk,2)&&!service.register_device(alias,1),"pending pointer and identity reserved");
        check(disk.geometry_calls==1&&alias.geometry_calls==0,"pending duplicate has no raw geometry query");
        check(!service.inspect(old.value())&&service.detach(old.value()).code()==StatusCode::not_found,"pending generation hides stale key");
        const auto parallel=service.register_device(other,2);
        check(parallel.ok()&&other.geometry_calls==1,"independent registration proceeds outside lock");
        check(service.register_device(full,3).status().code()==StatusCode::no_memory&&full.geometry_calls==0,"pending reservation occupies capacity");
        leave.store(true,std::memory_order_release);worker.join();
        check(result==StatusCode::invalid_argument,"invalid registration fails after callback");
        disk.geometry_callback={};disk.shape.block_count=32;
        const auto retry=service.register_device(disk,1);
        check(retry.ok(),"failed reservation releases pointer identity and capacity");
        check(!service.inspect(old.value()),"failed reservation preserves generation invalidation");
        check(service.detach(retry.value()).ok()&&service.detach(parallel.value()).ok(),"reservation registrations detach");
        check(service.register_device(disk,1,511).status().code()==StatusCode::invalid_argument,"invalid transfer reservation rolls back");
        check(service.register_device(disk,1,512).ok(),"transfer rollback releases reservation");
    }
    {
        Device a,b,c; Registry service; Registry::Session first,alias,readonly;
        check(!service.register_device(a,0),"zero identity");
        const auto key=service.register_device(a,1);check(key.ok(),"register");
        check(!service.register_device(a,2)&&!service.register_device(b,1),"duplicate physical/identity aliases");
        const auto other=service.register_device(b,2);check(other.ok()&&!service.register_device(c,3),"registration capacity");
        check(service.acquire(key.value(),{31,2},SessionAccess::writable,first).code()==StatusCode::invalid_argument,"partition bound");
        check(service.acquire(key.value(),{1,3},SessionAccess::writable,first).ok(),"partition owner");
        check(first.geometry().block_count==3&&first.geometry().writable,"relative geometry");
        check(service.acquire(key.value(),{8,3},SessionAccess::readonly,alias).code()==StatusCode::busy,"disjoint alias ownership");
        check(service.detach(key.value()).code()==StatusCode::busy,"owned detach");
        check(first.read_blocks(3,bytes).code()==StatusCode::invalid_argument&&a.reads==0,"relative bounds");
        check(first.read_blocks(0,std::span<std::byte>{bytes}.first(511)).code()==StatusCode::invalid_argument,"unaligned request");
        check(first.read_blocks(0,{}).code()==StatusCode::invalid_argument,"empty request");
        check(first.read_blocks(2,bytes).ok()&&a.last_sector==3,"partition translation");
        bytes.fill(std::byte{0x5a});check(first.write_blocks(0,bytes).ok(),"write");
        check(first.close().code()==StatusCode::busy&&a.flushes==0,"close never hidden-flushes");
        check(first.report().value().pending_writes,"pending durability");
        check(first.flush().ok()&&!first.report().value().pending_writes,"flush durable accounting");
        check(first.close().ok(),"drained close");
        check(service.acquire(key.value(),{0,32},SessionAccess::writable,first).code()==StatusCode::already_exists,"one-shot view");
        check(service.acquire(key.value(),{0,32},SessionAccess::readonly,readonly).ok(),"next readonly owner");
        check(first.read_blocks(0,bytes).code()==StatusCode::bad_descriptor,"old session serial");
        check(readonly.write_blocks(0,bytes).code()==StatusCode::denied&&readonly.flush().code()==StatusCode::denied,"readonly enforcement");
        check(readonly.close().ok()&&service.detach(key.value()).ok(),"detach after release");
        const auto replacement=service.register_device(c,3);check(replacement.ok(),"slot reuse");
        check(!service.inspect(key.value())&&service.acquire(key.value(),{0,32},SessionAccess::readonly,alias).code()==StatusCode::not_found,"stale registration generation");
        Registry independent;Registry::Session wrong;
        check(independent.acquire(replacement.value(),{0,32},SessionAccess::readonly,wrong).code()==StatusCode::not_found,"foreign registration");
    }
    for (int operation=0;operation<3;++operation)
    {
        Device disk;Registry service;Registry::Session session,next;
        const auto key=service.register_device(disk,1);check(key.ok(),"error register");
        check(service.acquire(key.value(),{0,32},SessionAccess::writable,session).ok(),"error acquire");
        disk.fail_read=operation==0;disk.fail_write=operation==1;disk.fail_flush=operation==2;
        const auto result=operation==0?session.read_blocks(0,bytes):operation==1?session.write_blocks(0,bytes):session.flush();
        check(!result&&service.inspect(key.value()).value().state==DeviceState::quarantined,"I/O error quarantine");
        const auto attempts=disk.reads+disk.writes+disk.flushes;
        check(!session.read_blocks(0,bytes)&&!session.write_blocks(0,bytes)&&!session.flush(),"quarantined requests");
        check(attempts==disk.reads+disk.writes+disk.flushes,"no calls after quarantine");
        check(service.detach(key.value()).code()==StatusCode::denied&&
              service.acquire(key.value(),{0,32},SessionAccess::readonly,next).code()==StatusCode::denied,"no unproved fencing escape");
        check(!service.register_device(disk,7),"quarantine retains raw ownership");
    }
    for(int operation=0;operation<3;++operation)
    {
        Device disk;Registry service;Registry::Session session;
        const auto key=service.register_device(disk,1);check(key.ok(),"geometry register");
        check(service.acquire(key.value(),{0,32},SessionAccess::writable,session).ok(),"geometry acquire");
        disk.change_after=true;
        const auto result=operation==0?session.read_blocks(0,bytes):operation==1?session.write_blocks(0,bytes):session.flush();
        check(!result&&service.inspect(key.value()).value().state==DeviceState::quarantined,"post-request geometry quarantine");
    }
    {
        Device disk;Registry service;Registry::Session session;
        const auto key=service.register_device(disk,1);check(key.ok(),"pregeometry register");
        check(service.acquire(key.value(),{0,32},SessionAccess::writable,session).ok(),"pregeometry acquire");
        disk.shape.block_count=31;
        check(!session.read_blocks(0,bytes)&&disk.reads==0,"geometry before request");
    }
    {
        Device disk;Registry service;const auto key=service.register_device(disk,1);check(key.ok(),"abandon register");
        {Registry::Session session;check(service.acquire(key.value(),{0,32},SessionAccess::writable,session).ok(),"abandon acquire");
         check(session.write_blocks(0,bytes).ok(),"abandon dirty write");}
        check(service.inspect(key.value()).value().state==DeviceState::quarantined&&disk.flushes==0,"dirty destructor quarantine");
    }
    {
        Device disk;Registry service;Registry::Session session;const auto key=service.register_device(disk,1);check(key.ok(),"integrity register");
        check(service.acquire(key.value(),{0,32},SessionAccess::writable,session).ok(),"integrity acquire");
        check(!session.abort({})&&session.abort(Status::io_error("checksum")).ok(),"integrity abort requires error");
        check(service.inspect(key.value()).value().first_error==StatusCode::io_error&&disk.writes==0&&disk.flushes==0,"integrity abort no I/O");
    }
    {
        Device disk;Registry service;Registry::Session session,second;const auto key=service.register_device(disk,1);check(key.ok(),"reentrant register");
        check(service.acquire(key.value(),{0,32},SessionAccess::writable,session).ok(),"reentrant acquire");
        disk.callback=[&] {
            check(session.read_blocks(0,bytes).code()==StatusCode::busy,"reentrant request exclusion");
            check(session.close().code()==StatusCode::busy&&service.detach(key.value()).code()==StatusCode::busy,"request pin");
            check(service.acquire(key.value(),{0,32},SessionAccess::readonly,second).code()==StatusCode::busy,"reentrant ownership");
            check(service.inspect(key.value()).value().in_flight,"metadata lock not held across device callback");
        };
        check(session.read_blocks(0,bytes).ok(),"reentrant completed read");
        disk.callback=[&] {check(!session.abandon(),"in-flight abandonment");check(service.inspect(key.value()).value().in_flight,"abandon retains pin");};
        check(!session.flush()&&!service.inspect(key.value()).value().in_flight,"aborted completion retires request pin");
        check(service.inspect(key.value()).value().state==DeviceState::quarantined,"aborted successful flush stays quarantined");
    }
    {
        Device disk;Registry service;Registry::Session session,competitor;const auto key=service.register_device(disk,1);check(key.ok(),"thread register");
        check(service.acquire(key.value(),{0,32},SessionAccess::writable,session).ok(),"thread acquire");
        std::atomic<bool> entered{},leave{};Status result;
        disk.callback=[&] {entered.store(true,std::memory_order_release);while(!leave.load(std::memory_order_acquire))std::this_thread::yield();};
        std::thread worker([&] {result=session.write_blocks(0,bytes);});
        while(!entered.load(std::memory_order_acquire))std::this_thread::yield();
        check(session.flush().code()==StatusCode::busy&&session.close().code()==StatusCode::busy,"concurrent request/close exclusion");
        check(service.acquire(key.value(),{0,32},SessionAccess::readonly,competitor).code()==StatusCode::busy,"concurrent owner exclusion");
        leave.store(true,std::memory_order_release);worker.join();check(result.ok(),"thread completed transfer");
        disk.callback={};check(session.close().code()==StatusCode::busy&&session.flush().ok()&&session.close().ok(),"thread durability fence");
    }
    {
        Device disk;disk.shape.writable=false;Registry service;Registry::Session session;
        const auto key=service.register_device(disk,1);check(key.ok(),"readonly device register");
        check(service.acquire(key.value(),{0,32},SessionAccess::writable,session).code()==StatusCode::denied,"readonly device admission");
        check(service.acquire(key.value(),{0,32},SessionAccess::readonly,session).ok()&&session.read_blocks(0,bytes).ok(),"readonly device read");
        check(session.close().ok()&&disk.writes==0&&disk.flushes==0,"readonly device release");
    }
    {
        Device disk;Registry service;const auto key=service.register_device(disk,1);check(key.ok(),"clean scope register");
        {Registry::Session session;check(service.acquire(key.value(),{0,32},SessionAccess::readonly,session).ok(),"clean scope acquire");}
        check(service.inspect(key.value()).value().state==DeviceState::ready&&service.detach(key.value()).ok(),"clean scope release");
        check(disk.reads==0&&disk.writes==0&&disk.flushes==0,"release discovery has no transfer I/O");
    }
    {
        Device disk;disk.origin=(u64{1}<<40)+3;disk.shape.block_count=disk.origin+32;
        Registry service;Registry::Session session;const auto key=service.register_device(disk,1,512);check(key.ok(),"u64 register");
        check(service.acquire(key.value(),{disk.origin+1,31},SessionAccess::writable,session).ok(),"u64 partition");
        check(session.write_blocks(30,bytes).ok()&&disk.last_sector==disk.origin+31,"u64 translation");
        std::array<std::byte,1024> large{};check(session.read_blocks(0,large).code()==StatusCode::invalid_argument,"maximum transfer admission");
        check(session.flush().ok()&&session.close().ok(),"u64 close");
    }
    std::printf("PASS hosted block sessions checks=%zu host_threads=1 native_smp=0\n",checks);
}
