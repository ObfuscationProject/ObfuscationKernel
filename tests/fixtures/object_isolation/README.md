# Parser fixtures only

These ten serial samples and ELF symbol layouts were retained from the actual
QEMU object-isolation V4 experiment. In this directory they are **hosted parser
fixtures**, not a new execution receipt. Native qualification requires building
and running `scripts/test_native_object_isolation.py` with pinned toolchains.
The independent original experiment and its artifact review remain outside the
source tree in the qualification archive. Corruptions test fail-closed admission.
