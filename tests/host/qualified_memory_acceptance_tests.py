#!/usr/bin/env python3
"""Evidence parser regressions; fixtures are archived actual QEMU output."""
import sys
from pathlib import Path
import unittest
import struct
sys.dont_write_bytecode=True
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'scripts'))
import test_native_qualified_memory as runner

class AcceptanceTests(unittest.TestCase):
    def fixture(self,arch='x86_64'):
        return (root/'tests/fixtures/qualified_memory'/f'{arch}.txt').read_bytes()
    def reject(self,data):
        with self.assertRaises(ValueError):runner.acceptance('x86_64',data)
    def elf(self,wide=False,big=False,physical=0x1000,memsize=0x9000):
        endian='>' if big else '<'
        header=struct.Struct(endian+('HHIQQQIHHHHHH' if wide else 'HHIIIIIHHHHHH'))
        segment=struct.Struct(endian+('IIQQQQQQ' if wide else 'IIIIIIII'))
        identity=b'\x7fELF'+bytes([2 if wide else 1,2 if big else 1,1])+bytes(9)
        phoff=16+header.size;fileoff=phoff+segment.size
        fields=(2,0,1,0x1000,phoff,0,0,16+header.size,segment.size,1,0,0,0)
        values=(1,6,fileoff,0x1000,physical,1,memsize,4096) if wide else (1,fileoff,0x1000,physical,1,memsize,6,4096)
        return identity+header.pack(*fields)+segment.pack(*values)+bytes(1)
    def test_arm32_linux_image(self):
        runner.acceptance('arm32',self.fixture('arm32-linux-image'))
    def test_elf_loaded_and_noload_all_layouts(self):
        for wide in (False,True):
            for big in (False,True):
                result=runner.loaded_elf_extents(self.elf(wide,big))
                self.assertEqual(result,{'virtual':(0x1000,0x9000),'physical':(0x1000,0x9000),'identity':True})
    def test_elf_nonidentity_remains_explicit(self):
        result=runner.loaded_elf_extents(self.elf(True,True,physical=0x2000))
        self.assertFalse(result['identity']);self.assertEqual(result['physical'],(0x2000,0x9000))
    def test_elf_truncation_and_overflow(self):
        for data in (self.elf()[:-1],self.elf(True,False,physical=0xfffffffffffff000,memsize=0x9000),b'bad'):
            with self.assertRaises(ValueError):runner.loaded_elf_extents(data)
    def test_all_ten_archived_guests(self):
        for arch in runner.profiles:
            with self.subTest(arch=arch):runner.acceptance(arch,self.fixture(arch),legacy_arm32=arch=='arm32')
    def test_missing_memory_result(self):
        self.reject(b'\n'.join(line for line in self.fixture().split(b'\n') if not line.startswith(b'MEMORY_RESULT')))
    def test_duplicate_memory_result(self):
        data=self.fixture();line=next(line for line in data.splitlines() if line.startswith(b'MEMORY_RESULT'))
        self.reject(data.replace(line,line+b'\n'+line))
    def test_false_page_touch_count(self):
        self.reject(self.fixture().replace(b'touched=0x0000000000000400',b'touched=0x0000000000000000'))
    def test_page_inside_loaded_payload(self):
        self.reject(self.fixture().replace(b'MEMORY_MANAGED base=0x0000000000500000',b'MEMORY_MANAGED base=0x0000000000100000'))
    def test_overlapping_partition(self):
        self.reject(self.fixture().replace(b'MEMORY_EXCLUDED base=0x0000000000900000',b'MEMORY_EXCLUDED base=0x0000000000800000'))
    def test_missing_exclusion(self):
        self.reject(b'\n'.join(line for line in self.fixture().split(b'\n') if not line.startswith(b'MEMORY_EXCLUDED base=0x0000000000900000')))
    def test_invalid_exclusion_reason(self):
        self.reject(self.fixture().replace(b'reason=0x0000000000000004',b'reason=0x0000000000000008'))
    def test_mislabelled_capacity_exclusion(self):
        self.reject(self.fixture().replace(b'reason=0x0000000000000004',b'reason=0x0000000000000003'))
    def test_overstated_entry_mapping(self):
        self.reject(self.fixture().replace(b'size=0x0000000040000000 ceiling=',b'size=0x0000000100000000 ceiling='))
    def test_truncated_completion(self):
        self.reject(self.fixture().split(b'FIRMWARE_DONE')[0]+b'FIRMWARE_DONE scope=actual-qemu-boot')
    def test_out_of_order_memory_evidence(self):
        lines=self.fixture().splitlines(keepends=True);index=next(i for i,line in enumerate(lines) if line.startswith(b'MEMORY_IMAGE'))
        image=lines.pop(index);lines.insert(0,image);self.reject(b''.join(lines))
    def test_unknown_memory_line(self):
        self.reject(self.fixture().replace(b'MEMORY_RESULT',b'MEMORY_UNKNOWN\nMEMORY_RESULT'))
    def test_managed_size_hostile(self):
        self.reject(self.fixture().replace(b'MEMORY_MANAGED base=0x0000000000500000 size=0x0000000000400000',b'MEMORY_MANAGED base=0x0000000000500000 size=0xffffffffffffffff'))

if __name__=='__main__':unittest.main()
