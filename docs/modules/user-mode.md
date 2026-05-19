# User Mode

`ok::user::UserSpaceManager` delegates transitions to `UserModeGateway`. The
baseline gateway is simulated and updates a CPU context to user mode.

`TransitionMode` keeps architecture entry/return variants visible:

- `simulated`: debug and freestanding profile transition state.
- `trap_return`: return-from-interrupt or return-from-exception path.
- `fast_return`: architecture fast-return path such as `sysret`-style flows.

Real architecture implementations will replace this with `iret`, `sysret`,
`eret`, `sret`, or equivalent return-from-exception sequences.
