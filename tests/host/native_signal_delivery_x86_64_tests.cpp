// Hosted delivery transaction tests: no scheduler, guest CPU or live sigreturn.
#include "ok/user/native_signal_delivery_x86_64.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace ok;
using Frame=user::native::X86SignalFrame;
using Delivery=user::native::X86SignalDelivery;
using DeliveryResult=Delivery::Result;
using Queue=sched::PendingSignals<64,8>;
using Mask=sched::SignalMask;
static usize checks{};
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"%d: %s\n",__LINE__,#x);std::abort();}}while(0)
constexpr u64 begin=0x40000000,end=begin+32768;
struct Store{u64 address{};usize requested{},copied{};};
struct Memory{
 std::array<std::byte,32768> bytes{};
 std::vector<Store> stores;
 usize fail_call{},partial{};
 Memory(){bytes.fill(std::byte{0xa5});}
 bool write(u64 address,std::span<const std::byte> in){
  const usize call=stores.size()+1;
  const bool mapped=address>=begin&&address<end&&in.size()<=end-address;
  const auto n=mapped?(call==fail_call?std::min(partial,in.size()):in.size()):0;
  stores.push_back({address,in.size(),n});
  if(n)std::copy_n(in.begin(),n,bytes.begin()+address-begin);
  return mapped&&n==in.size();
 }
 bool read(u64 address,std::span<std::byte> out){
  if(address<begin||address>=end||out.size()>end-address)return false;
  std::copy_n(bytes.begin()+address-begin,out.size(),out.begin());return true;
 }
 u64 get(u64 address,usize count){u64 value{};CHECK(address>=begin&&address+count<=end);for(usize i=0;i<count;++i)value|=static_cast<u64>(bytes[address-begin+i])<<(8*i);return value;}
 void put(u64 address,u64 value,usize count){CHECK(address>=begin&&address+count<=end);for(usize i=0;i<count;++i)bytes[address-begin+i]=std::byte(value>>(8*i));}
};
bool equal_frame(const Frame::Frame &a,const Frame::Frame &b){return a.registers==b.registers&&a.pc==b.pc&&a.sp==b.sp&&a.flags==b.flags&&a.fs_base==b.fs_base&&a.gs_base==b.gs_base&&a.fp_state==b.fp_state;}
struct Fixture{
 Queue pending;
 sched::SignalActionTable actions;
 Mask blocked{};
 sched::SignalAltstack altstack{};
 Frame::Frame frame{};
 Memory memory;
 u32 signal{0xaabbccdd};
 Fixture(){
  CHECK(pending.reset({40,41,42}));frame.pc=begin+256;frame.sp=end-16;frame.flags=0x250fd7;
  frame.fs_base=begin+512;frame.gs_base=begin+768;
  for(usize i=0;i<32;++i)frame.registers[i]=0x1122334455000000ULL+i;
  for(usize i=0;i<512;++i)frame.fp_state[i]=static_cast<u8>(i*7);
  frame.fp_state[24]=0x80;frame.fp_state[25]=0x1f;frame.fp_state[26]=frame.fp_state[27]=0;
 }
 void enqueue(u32 number,u32 sender=99){CHECK(pending.enqueue({number,-6,sender,100})==Queue::Enqueue::queued);}
 void caught(u32 number,u64 flags=Delivery::restorer){auto &a=*actions.find(number);a={};a.handler=begin+1024+number;a.restorer=begin+2048;a.flags=flags;}
 DeliveryResult deliver(){return Delivery::deliver(pending,actions,blocked,frame,altstack,begin,end,0xffbf,[&](u64 at,auto bytes){return memory.write(at,bytes);},signal);}
};
void supported_flags(){
 for(u64 bits=0;bits<16;++bits){
  const u64 flags=((bits&1)?Delivery::siginfo:0)|((bits&2)?Delivery::restorer:0)|((bits&4)?Delivery::nodefer:0)|((bits&8)?Delivery::resethand:0);
  for(u64 handler:{u64{0},u64{1},begin+64})CHECK(Delivery::supported({.handler=handler,.flags=flags})==(handler<=1||bool(flags&Delivery::restorer)));
  if(!(flags&Delivery::restorer))continue;
  Fixture f;f.caught(10,flags);f.enqueue(10);f.blocked.set(12);f.blocked.set(33);
  auto &a=*f.actions.find(10);a.mask.set(13);a.mask.set(9);a.mask.set(19);a.mask.set(128);
  if(bits&1)a.mask.set(10); // NODEFER does not remove the action's own mask bit.
  const auto before=f.frame;const auto oldmask=f.blocked;const auto oldaction=a;
  Queue::Delivery ticket;CHECK(f.pending.peek(f.blocked,ticket));
  CHECK(f.deliver()==DeliveryResult::handler&&f.signal==10);
  CHECK(f.memory.stores.size()==2&&f.memory.stores[0].requested==512&&f.memory.stores[1].requested==440);
  const auto at=f.frame.sp,fp=f.memory.stores[0].address;
  CHECK(at%16==8&&fp%64==0&&at+440<=fp&&fp+512<=before.sp-128);
  CHECK(f.memory.stores[1].address==at);
  // Literal Linux RT-frame offsets, independent of candidate offset constants.
  CHECK(f.memory.get(at,8)==oldaction.restorer&&f.memory.get(at+8,8)==6);
  CHECK(f.memory.get(at+168,8)==before.sp&&f.memory.get(at+176,8)==before.pc);
  CHECK(f.memory.get(at+216,8)==oldmask.words[0]&&f.memory.get(at+232,8)==fp&&f.memory.get(at+304,8)==oldmask.words[0]);
  CHECK(f.memory.get(at+312,4)==10&&f.memory.get(at+316,4)==0&&f.memory.get(at+320,4)==0xfffffffaU);
  CHECK(f.memory.get(at+328,4)==99&&f.memory.get(at+332,4)==100);
  CHECK(f.blocked.contains(12)&&f.blocked.contains(13)&&f.blocked.contains(33));
  CHECK(f.blocked.contains(10)==(!(flags&Delivery::nodefer)||bool(bits&1)));
  CHECK(!f.blocked.contains(9)&&!f.blocked.contains(19)&&!f.blocked.contains(128));
  CHECK(a.handler==((flags&Delivery::resethand)?0:oldaction.handler)&&a.flags==oldaction.flags&&a.mask==oldaction.mask&&a.restorer==oldaction.restorer);
  CHECK(f.frame.pc==oldaction.handler&&f.frame.registers[9]==10&&f.frame.registers[10]==at+312&&f.frame.registers[11]==at+8&&f.frame.registers[14]==0);
  for(usize i=0;i<32;++i)if(i!=9&&i!=10&&i!=11&&i!=14)CHECK(f.frame.registers[i]==before.registers[i]);
  CHECK(f.frame.fs_base==before.fs_base&&f.frame.gs_base==before.gs_base);
  CHECK(f.frame.flags==((before.flags&~u64{0x100|0x400|0x10000})|0x202));
  const auto reset=Frame::reset_fp();for(usize i=0;i<512;++i)CHECK(f.frame.fp_state[i]==static_cast<u8>(reset[i]));
  CHECK(!f.pending.consume(ticket)&&!f.pending.pending_mask().contains(10));
  // Existing interrupted red zone is untouched by either write.
  for(u64 pos=before.sp-128;pos<before.sp;++pos)CHECK(f.memory.bytes[pos-begin]==std::byte{0xa5});
  Frame::Restored restored;CHECK(Frame::decode(at+8,f.frame,begin,end,0xffbf,[&](u64 addr,auto bytes){return f.memory.read(addr,bytes);},restored));
  CHECK(restored.frame.registers==before.registers&&restored.frame.pc==before.pc&&restored.frame.sp==before.sp&&restored.mask==oldmask);
  CHECK(restored.frame.fs_base==before.fs_base&&restored.frame.gs_base==before.gs_base);
 }
 for(unsigned bit=0;bit<64;++bit)if(!(Delivery::supported_flags&(u64{1}<<bit))){Fixture f;f.caught(10,Delivery::restorer|(u64{1}<<bit));f.enqueue(10);const auto before=f.frame;const auto action=*f.actions.find(10);CHECK(!Delivery::supported(action));CHECK(f.deliver()==DeliveryResult::unsupported);CHECK(equal_frame(before,f.frame)&&*f.actions.find(10)==action&&f.pending.pending_mask().contains(10)&&f.memory.stores.empty());}
}
void no_handler_paths(){
 {Fixture f;const auto before=f.frame;CHECK(f.deliver()==DeliveryResult::none&&f.signal==0xaabbccdd&&equal_frame(before,f.frame)&&f.memory.stores.empty());}
 {Fixture f;f.enqueue(10);f.blocked.set(10);const auto before=f.frame;CHECK(f.deliver()==DeliveryResult::none&&f.signal==0xaabbccdd&&equal_frame(before,f.frame)&&f.memory.stores.empty()&&f.pending.pending_mask().contains(10));}
 for(u32 sig:{1U,9U,10U,11U,31U,32U,64U}){Fixture f;f.enqueue(sig);const auto before=f.frame;CHECK(f.deliver()==DeliveryResult::terminate&&f.signal==sig&&!f.pending.pending_mask().contains(sig)&&equal_frame(before,f.frame)&&f.memory.stores.empty());}
 for(u32 sig:{17U,18U,23U,28U}){Fixture f;f.enqueue(sig);const auto before=f.frame;CHECK(f.deliver()==DeliveryResult::ignored&&f.signal==sig&&!f.pending.pending_mask().contains(sig)&&equal_frame(before,f.frame)&&f.memory.stores.empty());}
 for(u32 sig:{19U,20U,21U,22U}){Fixture f;f.enqueue(sig);const auto before=f.frame;CHECK(f.deliver()==DeliveryResult::unsupported&&f.signal==sig&&f.pending.pending_mask().contains(sig)&&equal_frame(before,f.frame)&&f.memory.stores.empty());}
 {Fixture f;f.enqueue(64);f.actions.find(64)->handler=1;CHECK(f.deliver()==DeliveryResult::ignored&&!f.pending.pending_mask().contains(64)&&f.memory.stores.empty());}
 {Fixture f;f.enqueue(10);CHECK(f.pending.enqueue({10,0,111,222})==Queue::Enqueue::coalesced);f.caught(10);CHECK(f.deliver()==DeliveryResult::handler);CHECK(f.memory.get(f.frame.sp+328,4)==99);}
}
void nested_and_rt(){
 Fixture f;f.caught(10,Delivery::restorer|Delivery::nodefer);f.caught(35,Delivery::restorer|Delivery::nodefer);f.caught(34,Delivery::restorer|Delivery::nodefer);
 f.blocked.set(13);const auto initial=f.frame;const auto initial_mask=f.blocked;
 f.enqueue(35,101);f.enqueue(34,102);f.enqueue(34,103);f.enqueue(10,104);
 CHECK(f.deliver()==DeliveryResult::handler&&f.signal==10);const auto first=f.frame;const auto firstmask=f.blocked;
 CHECK(f.deliver()==DeliveryResult::handler&&f.signal==34&&f.memory.get(f.frame.sp+328,4)==102);const auto second=f.frame;
 CHECK(f.frame.sp+440<=first.sp-128);
 Frame::Restored out;CHECK(Frame::decode(second.sp+8,f.frame,begin,end,0xffbf,[&](u64 a,auto b){return f.memory.read(a,b);},out));
 CHECK(out.frame.pc==first.pc&&out.frame.sp==first.sp&&out.frame.registers==first.registers&&out.mask==firstmask);
 f.frame=out.frame;f.blocked=out.mask;
 CHECK(f.deliver()==DeliveryResult::handler&&f.signal==34&&f.memory.get(f.frame.sp+328,4)==103);
 CHECK(Frame::decode(f.frame.sp+8,f.frame,begin,end,0xffbf,[&](u64 a,auto b){return f.memory.read(a,b);},out));f.frame=out.frame;f.blocked=out.mask;
 CHECK(f.deliver()==DeliveryResult::handler&&f.signal==35&&f.memory.get(f.frame.sp+328,4)==101);
 CHECK(Frame::decode(f.frame.sp+8,f.frame,begin,end,0xffbf,[&](u64 a,auto b){return f.memory.read(a,b);},out));f.frame=out.frame;f.blocked=out.mask;
 CHECK(Frame::decode(first.sp+8,f.frame,begin,end,0xffbf,[&](u64 a,auto b){return f.memory.read(a,b);},out));
 CHECK(out.frame.pc==initial.pc&&out.frame.sp==initial.sp&&out.frame.registers==initial.registers&&out.mask==initial_mask);
 CHECK(f.pending.realtime_size()==0&&!f.pending.pending_mask().contains(10));
 // Repeated same-signal nesting with NODEFER, explicit edited PC/mask and TLS
 // bases from the current handler, without any active-frame provenance token.
 Fixture g;g.caught(12,Delivery::restorer|Delivery::nodefer);g.enqueue(12);CHECK(g.deliver()==DeliveryResult::handler);auto outer=g.frame;
 g.enqueue(12);CHECK(g.deliver()==DeliveryResult::handler);const auto at=g.frame.sp;g.memory.put(at+176,begin+4096,8);g.memory.put(at+304,~u64{0},8);
 g.frame.fs_base=begin+8192;g.frame.gs_base=begin+8256;
 CHECK(Frame::decode(at+8,g.frame,begin,end,0xffbf,[&](u64 a,auto b){return g.memory.read(a,b);},out));
 CHECK(out.frame.pc==begin+4096&&out.frame.sp==outer.sp&&out.frame.fs_base==begin+8192&&out.frame.gs_base==begin+8256);
 CHECK(out.mask.contains(64)&&!out.mask.contains(9)&&!out.mask.contains(19));
}
void failures(){
 for(u32 number:{10U,34U})for(usize call:{usize{1},usize{2}})for(usize partial:{usize{0},usize{1},usize{31},usize{219}}){
  Fixture f;f.caught(number,Delivery::restorer|Delivery::resethand);f.enqueue(number);f.blocked.set(12);
  if(number>=32)f.enqueue(number,101);
  const auto before=f.frame;const auto blocked=f.blocked;const auto actions=f.actions;const auto memory=f.memory.bytes;Queue::Delivery pending;CHECK(f.pending.peek({},pending));
  f.memory.fail_call=call;f.memory.partial=partial;CHECK(f.deliver()==DeliveryResult::terminate&&f.signal==11);
  CHECK(equal_frame(before,f.frame)&&f.blocked==blocked&&f.actions==actions);
  Queue::Delivery after;CHECK(f.pending.peek({},after)&&after==pending);
  CHECK(f.pending.realtime_size()==(number>=32?2:0));
  CHECK(f.memory.stores.size()==call);auto expected=memory;
  const auto fp=f.memory.stores[0].address;Frame::Placement place;CHECK(Frame::place(before.sp,begin,end,place));
  const auto image=Frame::encode(before,blocked,{}, {static_cast<i32>(number),-6,99,100},place,actions.find(number)->restorer,0xffbf);
  const auto first_count=call==1?partial:512;std::copy_n(image.fp.begin(),first_count,expected.begin()+fp-begin);
  if(call==2)std::copy_n(image.prefix.begin(),partial,expected.begin()+place.address-begin);
  CHECK(f.memory.bytes==expected); // Partial user bytes are not rolled back.
  if(number>=32){CHECK(f.pending.consume(after));CHECK(f.pending.peek({},after)&&after.info.signal==number&&after.info.sender_pid==101);}
 }
 for(u64 sp:{u64{0},begin,begin+1151,end+129,~u64{0}}){Fixture f;f.caught(10);f.enqueue(10);f.frame.sp=sp;const auto before=f.frame;const auto mask=f.blocked;const auto actions=f.actions;CHECK(f.deliver()==DeliveryResult::terminate&&f.signal==11);CHECK(equal_frame(before,f.frame)&&mask==f.blocked&&actions==f.actions&&f.memory.stores.empty()&&f.pending.pending_mask().contains(10));}
 // Delivery validates the destination after subtracting the red zone. Even
 // a captured SP just above the user window can yield a valid frame; the
 // handler must repair that saved SP before strict sigreturn validation.
 for(u64 sp:{end+1,end+128}){Fixture f;f.caught(10);f.enqueue(10);f.frame.sp=sp;
  CHECK(f.deliver()==DeliveryResult::handler&&f.signal==10&&f.memory.stores.size()==2);
  CHECK(f.memory.get(f.frame.sp+168,8)==sp);
  Frame::Restored out;auto read=[&](u64 a,auto b){return f.memory.read(a,b);};
  CHECK(!Frame::decode(f.frame.sp+8,f.frame,begin,end,0xffbf,read,out));
  f.memory.put(f.frame.sp+168,end-16,8);
  CHECK(Frame::decode(f.frame.sp+8,f.frame,begin,end,0xffbf,read,out)&&out.frame.sp==end-16);
 }
 // Consume failure is injected through an explicit queue policy, after valid
 // writes; callbacks never mutate the real queue or action table.
 struct FailingQueue{using Delivery=Queue::Delivery;Queue actual;usize consumes{};bool peek(const Mask &m,Delivery &d){return actual.peek(m,d);}bool consume(const Delivery &){++consumes;return false;}};
 for(u32 number:{10U,34U})for(u64 handler:{u64{0},u64{1},begin+1024}){Fixture f;FailingQueue pending;CHECK(pending.actual.reset({40,41,42}));CHECK(pending.actual.enqueue({number,-6,99,100})==Queue::Enqueue::queued);f.caught(number,Delivery::restorer|Delivery::resethand);f.actions.find(number)->handler=handler;const auto before=f.frame;const auto actions=f.actions;const auto mask=f.blocked;
  CHECK(Delivery::deliver(pending,f.actions,f.blocked,f.frame,f.altstack,begin,end,0xffbf,[&](u64 a,auto b){return f.memory.write(a,b);},f.signal)==DeliveryResult::corrupt);
  CHECK(pending.consumes==1&&pending.actual.pending_mask().contains(number)&&equal_frame(before,f.frame)&&actions==f.actions&&mask==f.blocked);
  CHECK(f.memory.stores.size()==(handler>1?2:0));
 }
}
int main(){supported_flags();no_handler_paths();nested_and_rt();failures();std::printf("PASS independent x86_64 signal delivery transaction checks=%zu hosted=1 guest=0 hardware_isolation=0 smp=0\n",checks);}
