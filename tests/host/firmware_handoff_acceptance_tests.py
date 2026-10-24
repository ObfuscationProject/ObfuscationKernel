#!/usr/bin/env python3
"""Synthetic serial acceptance faults, distinct from actual QEMU execution."""
from pathlib import Path
import unittest
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/"scripts"))
import test_native_firmware_handoff as run_native
stage=Path(__file__).resolve().parents[2]
class Acceptance(unittest.TestCase):
    def data(self,arch):return(stage/'tests/fixtures/firmware_handoff'/f'{arch}.txt').read_bytes()
    def bad(self,data,arch='x86_64'):
        with self.assertRaises(ValueError):run_native.acceptance(arch,data,legacy_arm32=arch=='arm32')
    def test_saved_actual_all_ten(self):
        for arch in run_native.profiles:run_native.acceptance(arch,self.data(arch),legacy_arm32=arch=='arm32')
    def test_arm32_linux_image(self):
        data=self.data('arm32-linux-image');run_native.acceptance('arm32',data)
        for old,new in [(b'cpsr=0x00000000400001d3',b'cpsr=0x00000000400003d3'),(b'sctlr=0x0000000000c50078',b'sctlr=0x0000000002c50078'),(b'values=0x0000000000000000',b'values=0x0000000000000001'),(b'0x00000000ffffffff',b'0x0000000000000000'),(b'cpsr=0x00000000400001d3',b'cpsr=0x00000000400001da'),(b'sctlr=0x0000000000c50078',b'sctlr=0x0000000000c50079'),(b'mpidr=0x0000000080000000',b'mpidr=0x0000000080000001')]:
            with self.subTest(old=old),self.assertRaises(ValueError):run_native.acceptance('arm32',data.replace(old,new))
        with self.assertRaises(ValueError):run_native.acceptance('arm32',self.data('arm32'))
    def test_duplicate_raw(self):
        b=self.data('x86_64');self.bad(b+b.splitlines(keepends=True)[0])
    def test_duplicate_result(self):
        b=self.data('x86_64');self.bad(b+next(l for l in b.splitlines(keepends=True) if l.startswith(b'FIRMWARE_RESULT')))
    def test_missing_done(self):self.bad(self.data('x86_64').replace(b'FIRMWARE_DONE',b'PARTIAL_DONE'))
    def test_short_record(self):self.bad(self.data('x86_64').replace(b'E820_RECORD base=0x0000000000000000',b'E820_RECORD base=0x0'))
    def test_duplicate_map(self):
        b=self.data('x86_64');self.bad(b+b'E820_MAP broken\n')
    def test_wrong_count(self):self.bad(self.data('x86_64').replace(b'E820_MAP count=0x0000000000000007',b'E820_MAP count=0x0000000000000006'))
    def test_failure(self):self.bad(self.data('x86_64')+b'FIRMWARE_FAIL injected\n')
    def test_wrong_source(self):self.bad(self.data('aarch64').replace(b'values=0x0000000048000000',b'values=0x0000000049000000'),'aarch64')
    def test_wrong_ram(self):self.bad(self.data('rv32').replace(b'FDT_RAM base=0x0000000080000000',b'FDT_RAM base=0x0000000040000000'),'rv32')
    def test_unsupported_read(self):self.bad(self.data('mips')+b'FIRMWARE_READ address=0x0000000080002000 size=0x0000000000000028\n','mips')
    def test_wrong_order(self):
        lines=self.data('x86_64').splitlines(keepends=True);result=next(l for l in lines if l.startswith(b'FIRMWARE_RESULT'));lines.remove(result);self.bad(result+b''.join(lines))
    def test_no_false_high(self):
        with self.assertRaises(ValueError):run_native.acceptance('i386',self.data('i386'),high=True)
if __name__=='__main__':unittest.main()
