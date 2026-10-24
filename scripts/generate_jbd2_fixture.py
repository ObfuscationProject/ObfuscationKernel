#!/usr/bin/env python3
"""Create a fixed JBD2 v3 MODEL fixture using the e2fsprogs CRC32C oracle.

Record layouts are transcribed from Linux 6.13.4 include/linux/jbd2.h.
This synthetic fixture is not a Linux crash-recovery interoperability test.
"""
import argparse
import ctypes
import ctypes.util
from pathlib import Path
import struct


BLOCK_SIZE = 1024
MAGIC = 0xC03B3998
UUID = bytes(range(16))


def build_fixture():
    library = ctypes.CDLL(ctypes.util.find_library('ext2fs') or 'libext2fs.so.2')
    crc = library.ext2fs_crc32c_le
    crc.argtypes = [ctypes.c_uint32, ctypes.c_void_p, ctypes.c_size_t]
    crc.restype = ctypes.c_uint32

    def checksum(seed, data):
        raw = bytes(data)
        return crc(seed, raw, len(raw))

    assert checksum(0xFFFFFFFF, b'123456789') == 0x1CF96D7C
    seed = checksum(0xFFFFFFFF, UUID)

    def put(block, offset, value):
        struct.pack_into('>I', block, offset, value)

    def record(kind, sequence):
        block = bytearray(BLOCK_SIZE)
        struct.pack_into('>III', block, 0, MAGIC, kind, sequence)
        return block

    def seal(block, offset, initial=seed):
        put(block, offset, 0)
        put(block, offset, checksum(initial, block))
        return block

    def payload_sum(sequence, block):
        return checksum(checksum(seed, struct.pack('>I', sequence)), block)

    def descriptor(sequence, tags):
        block = record(1, sequence)
        cursor = 12
        for index, (target, escaped, payload) in enumerate(tags):
            flags = (1 if escaped else 0) | (2 if index else 0) | (8 if index + 1 == len(tags) else 0)
            struct.pack_into('>IIII', block, cursor, target, flags, 0, payload_sum(sequence, payload))
            cursor += 16
            if index == 0:
                block[cursor:cursor + 16] = UUID
                cursor += 16
        return seal(block, BLOCK_SIZE - 4)

    def commit(sequence, seconds):
        block = record(2, sequence)
        struct.pack_into('>QI', block, 48, seconds, 123456789)
        return seal(block, 16)

    sb = record(4, 0)
    for offset, value in {12: BLOCK_SIZE, 16: 1024, 20: 1008, 24: 0xFFFFFFFF,
                          28: 1022, 40: 17, 64: 1}.items():
        put(sb, offset, value)
    sb[48:64] = UUID
    sb[80] = 4
    seal(sb, 252, 0xFFFFFFFF)
    first = bytearray((index * 7 + 17) % 256 for index in range(BLOCK_SIZE))
    escaped = bytearray((index * 11 + 19) % 256 for index in range(BLOCK_SIZE))
    escaped[:4] = bytes(4)
    reused = bytearray((index * 13 + 23) % 256 for index in range(BLOCK_SIZE))
    revoke = record(5, 0)
    put(revoke, 12, 20)
    put(revoke, 16, 40)
    seal(revoke, BLOCK_SIZE - 4)
    # Packed in traversal order. Journal positions: 1022,1023,1008,...,1014.
    blocks = [sb, descriptor(0xFFFFFFFF, [(40, False, first), (41, True, escaped)]), first,
              escaped, commit(0xFFFFFFFF, 100), revoke, descriptor(0, [(42, False, reused)]),
              reused, commit(0, 101), commit(0xFFFFFFFF, 100)]
    return b''.join(blocks)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    path = Path(__file__).resolve().parents[1] / 'tests/fixtures/jbd2_v3.bin'
    data = build_fixture()
    if args.check:
        if path.read_bytes() != data:
            raise SystemExit('JBD2 fixture differs from e2fsprogs-generated oracle')
        print('JBD2 synthetic golden fixture matches e2fsprogs CRC32C oracle')
    else:
        path.write_bytes(data)


if __name__ == '__main__':
    main()
