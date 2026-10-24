#include "ok/sched/pending_signals.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <vector>
using namespace ok;
using namespace ok::sched;
static usize checks{};
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"line=%d expression=%s\n",__LINE__,#x);std::abort();}}while(0)
static u64 random_state=0x62cb491837aecf91ULL;
static u32 random_value(){random_state^=random_state<<13;random_state^=random_state>>7;random_state^=random_state<<17;return static_cast<u32>(random_state);}

// Independent reference: one insertion-ordered vector, no separate standard
// array or RT compaction. A stable minimum selects signal priority and FIFO.
template<u32 Maximum,usize Capacity> struct Reference
{
    using Queue=PendingSignals<Maximum,Capacity>;
    using Ticket=typename Queue::Delivery;
    using Identity=typename Queue::Identity;
    Identity owner{};u64 sequence{};std::vector<Ticket> records;
    bool reset(Identity next){if(!next.process||!next.thread||!next.generation)return false;owner=next;records.clear();return true;}
    typename Queue::Enqueue enqueue(PendingSignalInfo info,const Queue *queue)
    {
        using E=typename Queue::Enqueue;
        if(!owner.process||info.signal<1||info.signal>Maximum||(info.code!=0&&info.code!=-6))return E::invalid;
        if(info.signal<32&&std::any_of(records.begin(),records.end(),[&](auto &r){return r.info.signal==info.signal;}))return E::coalesced;
        if(info.signal>=32&&realtime_size()==Capacity)return E::full;
        records.push_back({owner,++sequence,info,queue});return E::queued;
    }
    usize realtime_size()const{return std::count_if(records.begin(),records.end(),[](auto &r){return r.info.signal>=32;});}
    bool peek(const SignalMask &blocked,Ticket &ticket)const
    {
        const Ticket *selected=nullptr;
        const auto rank=[](u32 signal) {
            const bool urgent=signal==4||signal==5||signal==8||signal==11||
                signal==(Maximum==128?10U:7U)||signal==(Maximum==128?12U:31U);
            return urgent?signal:signal+Maximum;
        };
        for(auto &record:records)if(!blocked.contains(record.info.signal)&&(!selected||rank(record.info.signal)<rank(selected->info.signal)))selected=&record;
        if(!selected)return false;
        ticket=*selected;return true;
    }
    bool consume(const Ticket &ticket){auto found=std::find(records.begin(),records.end(),ticket);if(found==records.end())return false;records.erase(found);return true;}
    void discard(u32 signal){std::erase_if(records,[&](auto &r){return r.info.signal==signal;});}
    SignalMask mask()const{SignalMask result;for(auto &r:records)result.set(r.info.signal);return result;}
};

template<u32 Maximum,usize Capacity> void random_model()
{
    using Q=PendingSignals<Maximum,Capacity>;using Ticket=typename Q::Delivery;
    static_assert(!std::is_copy_constructible_v<Q>&&!std::is_copy_assignable_v<Q>);
    static_assert(!std::is_move_constructible_v<Q>&&!std::is_move_assignable_v<Q>);
    Q queue;Reference<Maximum,Capacity> model;std::vector<Ticket> old_tickets;
    for(usize step=0;step<12000;++step)
    {
        const auto choice=random_value()%12;
        SignalMask blocked{{(u64{random_value()}<<32)|random_value(),(u64{random_value()}<<32)|random_value()}};
        const u32 signal=random_value()%(Maximum+3);
        if(choice<5)
        {
            const PendingSignalInfo info{signal,choice==0?7:choice==1?-6:0,random_value(),random_value()};
            CHECK(queue.enqueue(info)==model.enqueue(info,&queue));
        }
        else if(choice==5){queue.discard(signal);model.discard(signal);}
        else if(choice==6||choice==7)
        {
            Ticket actual{},expected{};const bool have=queue.peek(blocked,actual);
            CHECK(have==model.peek(blocked,expected));CHECK(actual==expected);
            if(have){old_tickets.push_back(actual);if(choice==7)CHECK(queue.consume(actual)==model.consume(actual));}
        }
        else if(choice==8&&!old_tickets.empty())
        {
            auto ticket=old_tickets[random_value()%old_tickets.size()];
            if(random_value()%2)++ticket.info.sender_pid;
            CHECK(queue.consume(ticket)==model.consume(ticket));
        }
        else if(choice==9||choice==10)
        {
            // Deliberately reuse owner IDs to check reset ABA protection.
            const typename Q::Identity owner{choice==9?0ULL:1ULL,1,1+random_value()%3};
            CHECK(queue.reset(owner)==model.reset(owner));
        }
        CHECK(queue.pending_mask()==model.mask());CHECK(queue.realtime_size()==model.realtime_size());CHECK(queue.identity()==model.owner);
        Ticket actual{},expected{};CHECK(queue.peek(blocked,actual)==model.peek(blocked,expected));CHECK(actual==expected);
        if(step%31==0)
        {
            for(u32 selected=1;selected<=Maximum;++selected)
            {
                SignalMask only{{~u64{0},~u64{0}}};only.set(selected,false);actual={};expected={};
                CHECK(queue.peek(only,actual)==model.peek(only,expected));CHECK(actual==expected);
            }
        }
    }
    std::printf("PASS pending reference maximum=%u capacity=%zu operations=12000\n",Maximum,Capacity);
}

template<u32 Maximum> void boundaries()
{
    using Q=PendingSignals<Maximum,3>;using E=typename Q::Enqueue;using Ticket=typename Q::Delivery;
    const typename Q::Identity a{7,11,13},b{7,12,14};Q first,other;
    CHECK(first.enqueue({1,0,1,2})==E::invalid);CHECK(first.reset(a));CHECK(other.reset(a));
    CHECK(first.enqueue({31,0,1,2})==E::queued);CHECK(first.enqueue({31,-6,9,8})==E::coalesced);
    CHECK(other.enqueue({31,0,1,2})==E::queued);Ticket initial{};CHECK(first.peek({},initial));
    CHECK(initial.sequence==1&&initial.info.sender_pid==1&&initial.info.code==0);CHECK(!other.consume(initial));
    for(unsigned field=0;field<9;++field)
    {
        auto forged=initial;
        switch(field){case 0:++forged.owner.process;break;case 1:++forged.owner.thread;break;case 2:++forged.owner.generation;break;
        case 3:++forged.sequence;break;case 4:++forged.info.signal;break;case 5:++forged.info.sender_pid;break;
        case 6:++forged.info.sender_uid;break;case 7:forged.info.code=-6;break;default:forged.queue=nullptr;break;}
        CHECK(!first.consume(forged));
    }
    CHECK(!first.reset({0,11,13})&&!first.reset({7,0,13})&&!first.reset({7,11,0}));
    Ticket unchanged{};CHECK(first.peek({},unchanged)&&unchanged==initial);
    CHECK(first.reset(b));CHECK(first.reset(a));CHECK(first.enqueue(initial.info)==E::queued);CHECK(!first.consume(initial));
    CHECK(first.peek({},unchanged)&&unchanged.sequence==2);first.discard(31);CHECK(first.enqueue(initial.info)==E::queued);
    CHECK(!first.consume(unchanged));CHECK(first.peek({},unchanged));CHECK(first.consume(unchanged));CHECK(!first.consume(unchanged));
    CHECK(first.enqueue({Maximum,0,100,1})==E::queued);CHECK(first.enqueue({32,0,101,1})==E::queued);CHECK(first.enqueue({32,-6,102,1})==E::queued);
    CHECK(first.enqueue({33,0,103,1})==E::full);CHECK(first.enqueue({2,0,104,1})==E::queued);
    CHECK(first.peek({},unchanged)&&unchanged.info.signal==2);CHECK(first.consume(unchanged));
    SignalMask blocked;blocked.set(32);CHECK(first.peek(blocked,unchanged)&&unchanged.info.signal==Maximum);CHECK(first.consume(unchanged));
    CHECK(first.peek({},unchanged)&&unchanged.info.sender_pid==101);const auto earlier=unchanged;CHECK(first.consume(unchanged));
    CHECK(first.peek({},unchanged)&&unchanged.info.sender_pid==102);CHECK(!first.consume(earlier));
    first.discard(32);CHECK(first.realtime_size()==0&&first.pending_mask()==SignalMask{});
    unchanged=initial;CHECK(!first.peek({},unchanged)&&unchanged==initial);
    first.discard(0);first.discard(Maximum+1);CHECK(first.enqueue({Maximum+1,0,1,2})==E::invalid);
    CHECK(first.enqueue({1,-1,1,2})==E::invalid);CHECK(first.enqueue({1,1,1,2})==E::invalid);
    CHECK(first.enqueue({Maximum,-6,1,2})==E::queued);blocked={{~u64{0},~u64{0}}};CHECK(!first.peek(blocked,unchanged));
    CHECK(first.reset(a));CHECK(first.enqueue({1,0,1,1})==E::queued);CHECK(first.enqueue({11,-6,1,1})==E::queued);
    CHECK(first.peek({},unchanged)&&unchanged.info.signal==11);blocked={};blocked.set(11);
    CHECK(first.peek(blocked,unchanged)&&unchanged.info.signal==1);
    CHECK(first.reset(a));CHECK(first.enqueue({7,0,1,1})==E::queued);CHECK(first.enqueue({10,0,1,1})==E::queued);
    CHECK(first.peek({},unchanged)&&unchanged.info.signal==(Maximum==128?10U:7U));
    CHECK(first.reset(a));CHECK(first.enqueue({2,0,1,1})==E::queued);CHECK(first.enqueue({Maximum==128?12U:31U,0,1,1})==E::queued);
    CHECK(first.peek({},unchanged)&&unchanged.info.signal==(Maximum==128?12U:31U));
    std::printf("PASS pending boundaries maximum=%u\n",Maximum);
}

void exhaustion()
{
    using Q=PendingSignals<64,1,3>;using E=Q::Enqueue;Q queue;const Q::Identity a{1,2,3};CHECK(queue.reset(a));
    CHECK(queue.enqueue({32,0,1,1})==E::queued);CHECK(queue.enqueue({33,0,1,1})==E::full);Q::Delivery ticket;
    CHECK(queue.peek({},ticket)&&ticket.sequence==1);CHECK(queue.consume(ticket));
    CHECK(queue.enqueue({1,0,1,1})==E::queued);CHECK(queue.peek({},ticket)&&ticket.sequence==2);CHECK(queue.consume(ticket));
    CHECK(queue.enqueue({1,0,2,2})==E::queued);CHECK(queue.peek({},ticket)&&ticket.sequence==3);
    CHECK(queue.enqueue({1,-6,3,3})==E::coalesced);CHECK(queue.enqueue({2,0,1,1})==E::full);
    CHECK(queue.consume(ticket));CHECK(queue.reset({1,2,4}));CHECK(queue.enqueue({1,0,1,1})==E::full);
    CHECK(queue.reset(a));CHECK(queue.enqueue({32,0,1,1})==E::full);
    std::printf("PASS pending sequence exhaustion never wraps or resets\n");
}

void retirement()
{
    using Q = PendingSignals<64, 2>;
    Q queue;
    CHECK(queue.reset({1, 1, 1}));
    CHECK(queue.enqueue({32, 0, 2, 3}) == Q::Enqueue::queued);
    Q::Delivery old;
    CHECK(queue.peek({}, old));
    queue.retire();
    CHECK(queue.identity() == Q::Identity{});
    CHECK(queue.pending_mask() == SignalMask{} && !queue.realtime_size());
    CHECK(!queue.consume(old) && queue.enqueue({32, 0, 2, 3}) == Q::Enqueue::invalid);
    CHECK(queue.reset({1, 1, 2}));
    CHECK(queue.enqueue({32, 0, 2, 3}) == Q::Enqueue::queued);
    Q::Delivery next;
    CHECK(queue.peek({}, next) && next.sequence > old.sequence && !queue.consume(old));
}
int main()
{
    retirement();
    boundaries<64>();boundaries<128>();exhaustion();
    random_model<64,1>();random_model<64,4>();random_model<64,32>();
    random_model<128,1>();random_model<128,4>();random_model<128,32>();
    // SDK31 adds a typed child status to each bounded record.
    static_assert(sizeof(PendingSignals<64>)<=2072&&sizeof(PendingSignals<128>)<=2072);
    std::printf("PASS pending checks=%zu queue_bytes=%zu ticket_bytes=%zu hosted=1 guest=0\n",checks,sizeof(PendingSignals<64>),sizeof(PendingSignals<64>::Delivery));
}
