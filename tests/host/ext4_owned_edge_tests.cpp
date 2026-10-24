#include "ext4_owned_support.hpp"
#include <memory>
using namespace owned_fixture;
using Owner=Ext4OwnedTransaction<>;
int main(int argc,char**argv)
{
 if(argc!=2)return 2;
 std::ifstream input(argv[1],std::ios::binary);const Bytes clean((std::istreambuf_iterator<char>(input)),{});
 const usize target=inode_offset(clean,12)/1024,j=journal(clean);
 {
  auto original=clean;big32(original,target*1024,jbd2::magic);
  Disk disk(original);Owner::Registry registry;auto owner_storage = std::make_unique<Owner>(); auto &owner = *owner_storage;const auto key=registry.register_device(disk,1);
  status(owner.open(registry,key.value(),{0,original.size()/512}),"escaped open");status(owner.prepare(update),"escaped prepare");status(owner.commit(),"escaped commit");
  auto expected=published_oracle(original);big32(expected,j+3072,0);big32(expected,j+1024+48,11);linux_payload_crc(expected,true);
  check(disk.durable==expected,"escaped exact publication");
  Owner::Pin pin;std::array<std::byte,1024> bytes{};status(owner.pin(target,pin),"escaped pin");status(pin.read(0,bytes),"escaped canonical");
  check(bytes[0]==std::byte{0xc0}&&bytes[1]==std::byte{0x3b}&&bytes[2]==std::byte{0x39}&&bytes[3]==std::byte{0x98},"escaped restored canonical magic");
  status(pin.release(),"escaped release");status(owner.checkpoint(),"escaped checkpoint");
  auto done=completed_oracle(expected);big32(done,target*1024,jbd2::magic);check(disk.durable==done,"escaped exact checkpoint");status(owner.close(),"escaped close");
 }
 {
  constexpr usize prefix=11*512,suffix=19*512;
  Bytes guard(prefix+clean.size()+suffix,0x9a);std::copy(clean.begin(),clean.end(),guard.begin()+prefix);
  Disk disk(guard);Owner::Registry registry;auto owner_storage = std::make_unique<Owner>(); auto &owner = *owner_storage;const auto key=registry.register_device(disk,1);
  status(owner.open(registry,key.value(),{11,clean.size()/512}),"guard open");status(owner.prepare(update),"guard prepare");status(owner.commit(),"guard commit");status(owner.checkpoint(),"guard checkpoint");status(owner.close(),"guard close");
  auto expected=guard;const auto complete=completed_oracle(published_oracle(clean));std::copy(complete.begin(),complete.end(),expected.begin()+prefix);
  check(disk.durable==expected,"physical prefix and suffix unchanged");
 }
 std::puts("PASS hosted owned ext4 edges cases=2 escaped_logical_images=1 guarded_partition=1");
}
