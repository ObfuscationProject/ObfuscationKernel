# User Mode

`ok::user::UserSpaceManager` delegates transitions to `UserModeGateway`. The
baseline gateway is simulated and updates a CPU context to user mode.

Real architecture implementations will replace this with `iret`, `sysret`,
`eret`, `sret`, or equivalent return-from-exception sequences.

