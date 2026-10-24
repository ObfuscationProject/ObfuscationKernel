"""Validate exact peer-futex execution records; hosted models cannot satisfy this gate."""
import re
SETUP='OK_PRIVATE_FUTEX_PEERS phase=setup scope=actual-user threads=4 shared_space=1 clone=0 pthread=0 smp=0'
PASS='OK_PRIVATE_FUTEX_PEERS status=pass version=1 scope=actual-user rounds=2 waiters=6 wakes=6 parks=8 controls=4 threads=4 remaining_objects=0 clone=0 pthread=0 smp=0 physical=0'
TRAP=re.compile(r'OK_PRIVATE_FUTEX_TRAP tid=([1-9][0-9]*) role=([0-3]) op=(128|129) address=(0x[0-9a-f]+) value=([0-9]+)')
def validate(serial):
 lines=serial.replace('\r','').splitlines()
 if any('OK_TEST_FAIL' in l or 'status=fail' in l or 'fault=contained' in l for l in lines):raise ValueError('guest failure record')
 records=[l for l in lines if l.startswith('OK_PRIVATE_FUTEX')]
 if not records or records[0]!=SETUP or records[-1]!=PASS:raise ValueError('missing exact setup/pass records')
 tids={};word=None;round=0;admitted=False;waits=set();parks=set();wakes=[];traps=0
 for line in records[1:-1]:
  if line==f'OK_PRIVATE_FUTEX_PEERS phase=admitted round={round} waiters=3':
   if admitted or waits!={0,1,2} or parks or wakes:raise ValueError('admission did not follow three real waits')
   admitted=True;continue
  if line==f'OK_PRIVATE_FUTEX_PEERS phase=round-complete round={round} results=0,0,0 wakes=1,1,1 changed_word=99':
   if not admitted or parks!={0,1,2,3} or wakes!=[0,1,4294967295]:raise ValueError('round lacks actual wake/return records')
   round+=1;admitted=False;waits=set();parks=set();wakes=[];continue
  match=TRAP.fullmatch(line)
  if not match or round>=2:raise ValueError('unrecognized, duplicated or out-of-phase record')
  tid,role,op,address,value=match.groups();tid,role,op,address,value=int(tid),int(role),int(op),int(address,16),int(value)
  if role in tids and tids[role]!=tid:raise ValueError('thread identity changed')
  if any(r!=role and t==tid for r,t in tids.items()):raise ValueError('roles alias one thread')
  tids[role]=tid;traps+=1
  if word is None:
   if role==3 or op!=128 or value!=7:raise ValueError('first record is not waiter admission')
   word=address
  if op==128 and address==word and role<3 and value==7 and not admitted and role not in waits:waits.add(role)
  elif op==129 and address==word and role==3 and admitted and len(wakes)<3:
   if value!=[0,1,4294967295][len(wakes)]:raise ValueError('wrong wake count order')
   wakes.append(value)
  elif op==128 and address==word+4 and value==0 and admitted and role not in parks:parks.add(role)
  else:raise ValueError('unexpected or replayed user trap')
 if round!=2 or traps!=20 or len(tids)!=4 or waits or parks or wakes:raise ValueError('incomplete peer execution')
 return {'rounds':2,'traps':20,'threads':4,'actual_user_wakes':6,'control_completions':4,'clone':False,'pthread':False,'smp':False,'physical':False,'thread_ids':tids}
