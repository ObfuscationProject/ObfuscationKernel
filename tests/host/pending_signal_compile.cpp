#include "ok/sched/pending_signals.hpp"
using namespace ok;
using namespace ok::sched;
template<u32 Maximum> bool instantiate(PendingSignals<Maximum> &queue,
    typename PendingSignals<Maximum>::Identity identity,PendingSignalInfo info,
    SignalMask blocked,typename PendingSignals<Maximum>::Delivery &ticket)
{
    if(!queue.reset(identity))return false;
    const auto result=queue.enqueue(info);
    if(result!=PendingSignals<Maximum>::Enqueue::queued)return false;
    if(!queue.peek(blocked,ticket))return false;
    const bool consumed=queue.consume(ticket);queue.discard(info.signal);
    return consumed&&queue.pending_mask()==SignalMask{}&&queue.realtime_size()==0&&queue.identity()==identity;
}
template bool instantiate<64>(PendingSignals<64>&,PendingSignals<64>::Identity,PendingSignalInfo,SignalMask,PendingSignals<64>::Delivery&);
template bool instantiate<128>(PendingSignals<128>&,PendingSignals<128>::Identity,PendingSignalInfo,SignalMask,PendingSignals<128>::Delivery&);
// SDK31 adds a typed child status to each bounded record.
static_assert(sizeof(PendingSignals<64>)<=2072&&sizeof(PendingSignals<128>)<=2072);
extern "C" [[gnu::used,gnu::section(".pending_sizes")]] const u32 pending_sizes[]={sizeof(PendingSignals<64>),sizeof(PendingSignals<128>),sizeof(PendingSignals<64>::Delivery)};
