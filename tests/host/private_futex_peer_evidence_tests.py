import sys,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts'))
from private_futex_peer_evidence import validate,PASS
class PeerEvidence(unittest.TestCase):
 def setUp(self):self.serial=(ROOT/'tests/fixtures/private_futex_peers/x86_64.serial').read_text()
 def rejects(self,value):
  with self.assertRaises(ValueError):validate(value)
 def test_real_trace(self):self.assertEqual(validate(self.serial)['actual_user_wakes'],6)
 def test_pass_alone(self):self.rejects(PASS+'\n')
 def test_duplicate_pass(self):self.rejects(self.serial+PASS+'\n')
 def test_missing_trap(self):self.rejects('\n'.join(l for i,l in enumerate(self.serial.splitlines()) if i!=1))
 def test_replayed_trap(self):self.rejects(self.serial.replace(self.serial.splitlines()[1],self.serial.splitlines()[1]+'\n'+self.serial.splitlines()[1],1))
 def test_wrong_count(self):self.rejects(self.serial.replace('value=4294967295','value=2',1))
 def test_tid_alias(self):self.rejects(self.serial.replace('tid=58','tid=57'))
 def test_failure_after_pass(self):self.rejects(self.serial+'OK_TEST_FAIL injected\n')
 def test_hosted_claim(self):self.rejects(self.serial.replace('scope=actual-user','scope=hosted'))
 def test_wrong_control_key(self):self.rejects(self.serial.replace('address=0x000000004001000c','address=0x0000000040010010',1))
if __name__=='__main__':unittest.main()
