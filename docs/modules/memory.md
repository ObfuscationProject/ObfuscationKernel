# Memory Management

`ok::memory::FrameAllocator` consumes a firmware-style memory map and tracks
fixed-size physical frames. `LinearAddressSpace` is a placeholder address-space
implementation used by debug kernel tests.

The next real architecture step is to replace `LinearAddressSpace` with
page-table implementations per architecture while preserving the generic
`AddressSpace` interface.
