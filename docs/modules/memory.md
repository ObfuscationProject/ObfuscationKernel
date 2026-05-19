# Memory Management

`ok::memory::FrameAllocator` consumes a firmware-style memory map and tracks
fixed-size physical frames. `LinearAddressSpace` is a placeholder address-space
implementation used by debug kernel tests.

`TranslationMode` records the selected memory-management profile:

- `linear`: early bring-up and tests use a bounded software mapping table.
- `paged`: future hardware page-table backends.
- `higher_half`: future kernel virtual base split from physical load address.

The next real architecture step is to replace `LinearAddressSpace` with
page-table implementations per architecture while preserving the generic
`AddressSpace` interface.
