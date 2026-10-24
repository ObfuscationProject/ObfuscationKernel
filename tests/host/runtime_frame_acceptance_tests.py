"""Parser regressions over retained serial evidence, not guest execution."""
import sys,unittest
from pathlib import Path
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'scripts'))
from test_native_runtime_frames import acceptance
class FrameEvidenceTests(unittest.TestCase):
    def sample(self,arch='i386'):return (root/'tests/fixtures/runtime_frames'/(arch+'.txt')).read_bytes()
    def reject(self,data,arch='i386'):
        with self.assertRaises(ValueError):acceptance(arch,data,legacy_arm32=arch=='arm32')
    def test_all_ten_fixtures(self):
        for arch in ('i386','x86_64','aarch64','arm32','rv64','rv32','loongarch64','mips','mips64','ppc'):
            with self.subTest(arch=arch):acceptance(arch,self.sample(arch),legacy_arm32=arch=='arm32')
    def test_arm32_linux_image(self):
        data=self.sample('arm32-linux-image');acceptance('arm32',data)
        for old,new in [(b'size=0x0000000010000000 ceiling=',b'size=0x0000000020000000 ceiling='),(b'executed=0x0000000000000001',b'executed=0x0000000000000000')]:
            with self.assertRaises(ValueError):acceptance('arm32',data.replace(old,new))
    def test_missing_native(self):
        self.reject(b'\n'.join(line for line in self.sample().split(b'\n') if not line.startswith(b'FRAME_NATIVE')))
    def test_duplicate_native(self):
        data=self.sample();line=next(x for x in data.splitlines() if x.startswith(b'FRAME_NATIVE'))
        self.reject(data.replace(line,line+b'\n'+line))
    def test_failed_status(self):self.reject(self.sample().replace(b'FRAME_NATIVE status=0x0000000000000000',b'FRAME_NATIVE status=0x0000000000000001'))
    def test_missing_execution(self):self.reject(self.sample().replace(b'executed=0x0000000000000001',b'executed=0x0000000000000000'))
    def test_missing_reclamation(self):self.reject(self.sample().replace(b'free=0x0000000000000400',b'free=0x00000000000003ff'))
    def test_wrong_capacity(self):self.reject(self.sample().replace(b'total=0x0000000000000400',b'total=0x0000000000000401'))
    def test_old_i386_entry_policy(self):self.reject(self.sample().replace(b'MEMORY_POLICY base=0x0000000000000000 size=0x0000000040000000',b'MEMORY_POLICY base=0x0000000000000000 size=0x0000000100000000'))
    def test_frame_result_after_completion(self):
        for arch in ('i386','arm32'):
            data=self.sample(arch);line=next(x for x in data.splitlines() if x.startswith(b'FRAME_NATIVE'))
            self.reject(data.replace(line+b'\n',b'')+line+b'\n',arch)
    def test_unsupported_claims_allocation(self):
        self.reject(self.sample('arm32').replace(b'total=0x0000000000000000',b'total=0x0000000000000001'),'arm32')
    def test_unsupported_claims_execution(self):
        self.reject(self.sample('arm32').replace(b'executed=0x0000000000000000',b'executed=0x0000000000000001'),'arm32')
    def test_bad_completion(self):self.reject(self.sample().replace(b'FIRMWARE_DONE scope=actual-qemu-boot\n',b'FIRMWARE_DONE scope=actual-qemu-boot'))
if __name__=='__main__':unittest.main()
