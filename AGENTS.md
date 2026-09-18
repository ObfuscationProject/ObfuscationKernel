# Kernel development contract

- Preserve all ten architecture profiles and xmake/freestanding C++23.
- Core mechanisms, native module/driver interfaces, essential platform drivers,
  the basic compositor and kernel diagnostics belong in this repository.
- Distribution desktop/session policy, external modules/drivers, Linux driver
  adaptation and ordinary applications belong in ObfuscationOS.
- Export architecture/mode-specific Kernel SDK artifacts. ObfuscationOS must not
  embed this repository or compile these sources from its build.
- Use constrained templates/static dispatch for CPU operations, scheduling
  policies and other high-frequency paths. Use runtime polymorphism only where
  implementations are selected at runtime, such as device/module service edges.
- Keep model coverage distinct from hardware execution and userland integration.
  Do not claim process isolation, AP startup, IRQ delivery or ELF execution from
  metadata/state checks.
- Follow docs/DEVELOPMENT.md. Move tests with moved code and validate standalone
  kernel boot plus OS SDK consumption when changing the integration boundary.
