"""Synthetic ELF parser controls only; this source draft has not been executed."""

import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest


HARNESS = Path(__file__).with_name("run_terminal_poweroff_tests_v1.py")
SPEC = importlib.util.spec_from_file_location("terminal_poweroff_harness_v1", HARNESS)
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


def fixture(enabled=True, duplicate=False, duplicate_table=False):
    """A small linked-shaped ELF fixture, never an executable test program."""
    image = bytearray(64)
    positions = {}

    def append(label, data, alignment=1):
        image.extend(b"\0" * ((-len(image)) % alignment))
        positions[label] = len(image)
        image.extend(data)
        return positions[label], len(data)

    text = runner.HOLD_BYTES + bytes(12) + runner.WRAPPER_BYTES
    text_offset, text_size = append("text", text, 16)
    other_offset, other_size = append("other", runner.WRAPPER_BYTES, 16)
    names = b"\0" + b"\0".join(name.encode("ascii") for name in (runner.HOLD, runner.WRAPPER, runner.OUTW)) + b"\0"
    strings_offset, strings_size = append("strings", names)
    positions["strings-size"] = strings_size
    entries = [("", 0, 0, 0, 0), (runner.HOLD, 0x12, 1, 0x401000, 4)]
    if enabled:
        entries += [(runner.WRAPPER, 0x12, 1, 0x401010, 9),
                    (runner.OUTW, 0x10, 1, 0x401016, 0)]
    if duplicate:
        entries.append((runner.WRAPPER, 0x12, 1, 0x401010, 9))
    symbols = bytearray()
    for name, info, owner, address, size in entries:
        name_offset = names.index(name.encode("ascii") + b"\0") if name else 0
        positions["symbol-" + name] = len(symbols)
        symbols.extend(struct.pack("<IBBHQQ", name_offset, info, 0, owner, address, size))
    symbols_offset, symbols_size = append("symbols", symbols, 8)
    section_names = b"\0.text\0.other\0.strtab\0.symtab\0.shstrtab\0.dynsym\0"
    section_names_offset, section_names_size = append("section-names", section_names)
    sections = [(0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]

    def section(name, kind, flags, address, offset, size, link=0, info=0, alignment=1, width=0):
        sections.append((section_names.index(name.encode("ascii") + b"\0"), kind, flags,
                         address, offset, size, link, info, alignment, width))

    section(".text", 1, 6, 0x401000, text_offset, text_size, alignment=16)
    section(".other", 1, 6, 0x402000, other_offset, other_size, alignment=16)
    section(".strtab", 3, 0, 0, strings_offset, strings_size)
    section(".symtab", 2, 0, 0, symbols_offset, symbols_size, link=3, info=1, alignment=8, width=24)
    section(".shstrtab", 3, 0, 0, section_names_offset, section_names_size)
    if duplicate_table:
        section(".dynsym", 11, 0, 0, symbols_offset, symbols_size, link=3, info=1, alignment=8, width=24)
    section_table = b"".join(struct.pack("<IIQQQQIIQQ", *entry) for entry in sections)
    table_offset, _ = append("section-table", section_table, 8)
    identity = b"\x7fELF\x02\x01\x01" + bytes(9)
    struct.pack_into("<16sHHIQQQIHHHHHH", image, 0, identity, 2, 62, 1, 0x401000,
                     0, table_offset, 0, 64, 0, 0, 64, len(sections), 5)
    return image, positions


def symbol_field(image, positions, name, field, value):
    offsets = {"name": (0, "I"), "info": (4, "B"), "section": (6, "H"),
               "address": (8, "Q"), "size": (16, "Q")}
    delta, format_code = offsets[field]
    offset = positions["symbols"] + positions["symbol-" + name] + delta
    struct.pack_into("<" + format_code, image, offset, value)


def section_field(image, positions, index, field, value):
    offsets = {"kind": (4, "I"), "flags": (8, "Q"), "address": (16, "Q"),
               "offset": (24, "Q"), "size": (32, "Q"), "link": (40, "I"), "width": (56, "Q")}
    delta, format_code = offsets[field]
    struct.pack_into("<" + format_code, image, positions["section-table"] + index * 64 + delta, value)


class StaticElfControls(unittest.TestCase):
    def inspect(self, image, enabled=True):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "linked-shaped-fixture.elf"
            path.write_bytes(image)
            return runner.LinkedElf(path).check_terminal(enabled)

    def refused(self, image, enabled=True):
        with self.assertRaises(ValueError):
            self.inspect(image, enabled)

    def test_enabled_fixture_proves_exact_interior_label_contract(self):
        image, _ = fixture()
        result = self.inspect(image)
        self.assertEqual(result["wrapper"]["bytes"], "6689fa6689f066efc3")
        self.assertEqual(result["outw"]["address"], result["wrapper"]["address"] + 6)
        self.assertEqual(result["outw"]["size"], 0)

    def test_disabled_fixture_requires_an_unstripped_symbol_table(self):
        image, positions = fixture(enabled=False)
        self.inspect(image, enabled=False)
        section_field(image, positions, 4, "kind", 0)
        self.refused(image, enabled=False)

    def test_header_and_section_ranges(self):
        for change in ("short-header", "class", "endian", "machine", "short-sections", "section-offset"):
            image, positions = fixture()
            if change == "short-header": image = image[:32]
            elif change == "class": image[4] = 1
            elif change == "endian": image[5] = 2
            elif change == "machine": struct.pack_into("<H", image, 18, 183)
            elif change == "short-sections": image = image[:-1]
            elif change == "section-offset": section_field(image, positions, 1, "offset", len(image) + 1)
            with self.subTest(change=change): self.refused(image)

    def test_symbol_and_string_table_ranges(self):
        for change in ("entry-size", "bad-link", "bad-name", "short-symbols", "unterminated-name"):
            image, positions = fixture()
            if change == "entry-size": section_field(image, positions, 4, "width", 23)
            elif change == "bad-link": section_field(image, positions, 4, "link", 999)
            elif change == "bad-name": symbol_field(image, positions, runner.WRAPPER, "name", 0xffffffff)
            elif change == "short-symbols": section_field(image, positions, 4, "size", 95)
            elif change == "unterminated-name":
                # OUTW is the final string; remove its terminator from the admitted range.
                section_field(image, positions, 3, "size", positions["strings-size"] - 1)
            with self.subTest(change=change): self.refused(image)

    def test_missing_duplicate_and_cross_table_symbols(self):
        image, _ = fixture(enabled=False)
        self.refused(image)
        image, _ = fixture(duplicate=True)
        self.refused(image)
        image, _ = fixture(duplicate_table=True)
        self.refused(image)

    def test_wrapper_binding_type_size_and_file_mapping(self):
        for field, value in (("info", 0x02), ("info", 0x10), ("size", 8),
                             ("section", 0), ("section", 0xfff1), ("section", 999),
                             ("address", 0x400fff), ("address", 0x401100)):
            image, positions = fixture()
            symbol_field(image, positions, runner.WRAPPER, field, value)
            with self.subTest(field=field, value=value): self.refused(image)
        image, positions = fixture()
        section_field(image, positions, 1, "flags", 4)
        self.refused(image)
        image, positions = fixture()
        section_field(image, positions, 1, "size", 24)
        self.refused(image)

    def test_observation_label_binding_type_size_and_offsets(self):
        for field, value in (("info", 0), ("info", 0x12), ("size", 2),
                             ("address", 0x401015), ("address", 0x401017)):
            image, positions = fixture()
            symbol_field(image, positions, runner.OUTW, field, value)
            with self.subTest(field=field, value=value): self.refused(image)
        image, positions = fixture()
        symbol_field(image, positions, runner.OUTW, "section", 2)
        symbol_field(image, positions, runner.OUTW, "address", 0x402006)
        self.refused(image)

    def test_instruction_mutations_and_disabled_symbol_injection(self):
        for delta in (0, 1, 6, 7, 8):
            image, positions = fixture()
            image[positions["text"] + 16 + delta] ^= 1
            with self.subTest(wrapper_byte=delta): self.refused(image)
        image, _ = fixture()
        self.refused(image, enabled=False)
        image, _ = fixture(enabled=False, duplicate=True)  # Wrapper only.
        self.refused(image, enabled=False)
        image, positions = fixture()
        symbol_field(image, positions, runner.WRAPPER, "name", 0)  # OUTW label only.
        self.refused(image, enabled=False)


if __name__ == "__main__":
    unittest.main()
