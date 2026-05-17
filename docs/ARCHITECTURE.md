# Architecture Overview

The kernel is split into narrow C++23 modules with virtual interfaces at hardware
or policy boundaries and concepts at compile-time extension points.

## Layers

- `include/ok/arch`: CPU profile traits and architecture operation interface.
- `include/ok/interrupt`: interrupt handler registration and dispatch.
- `include/ok/memory`: physical frame allocation and address-space interface.
- `include/ok/sched`: process/thread model and scheduler policy.
- `include/ok/ipc`: bounded message channels.
- `include/ok/syscall`: POSIX-oriented syscall table and handlers.
- `include/ok/driver`: driver base class and basic console/timer/block drivers.
- `include/ok/fs`: RAM-backed VFS node model.
- `include/ok/user`: user-mode transition gateway.
- `include/ok/core`: kernel composition and shared types.

## C++ Design Rules

Inheritance is used for runtime polymorphism where implementations are expected
to differ by machine, bus, device, or policy. Concepts are used for local,
zero-overhead extension points such as function-backed interrupt handlers,
syscall handlers, serializable IPC payloads, and driver registration.

RTTI is intentionally enabled. Kernel code may use `dynamic_cast` when crossing
a generic interface boundary and when the alternative would be unsafe manual type
tags.

## Boot Flow

`ok::Kernel::boot` performs the current simulated boot sequence:

1. Select architecture operations from `arch_target`.
2. Initialize physical memory from a memory map.
3. Register and start built-in drivers.
4. Register timer interrupt and baseline syscalls.
5. Create and schedule the idle process.
6. Create `/tmp/kernel.log` in the RAM VFS.

The smoke suite then validates one operation from every core module.

