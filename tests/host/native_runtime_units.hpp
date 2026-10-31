#pragma once
// Hosted tests compile the native runtime as one translation unit so they can
// reach its private state. Runners therefore exclude src/user/runtime.cpp and
// src/user/runtime/*.cpp from separate compilation.
#include "../../src/user/runtime.cpp"
#include "../../src/user/runtime/loader.cpp"
#include "../../src/user/runtime/vm.cpp"
#include "../../src/user/runtime/wait.cpp"
#include "../../src/user/runtime/futex.cpp"
#include "../../src/user/runtime/pipe.cpp"
#include "../../src/user/runtime/pty.cpp"
#include "../../src/user/runtime/net.cpp"
#include "../../src/user/runtime/signals.cpp"
#include "../../src/user/runtime/timers.cpp"
#include "../../src/user/runtime/interrupts.cpp"
#include "../../src/user/runtime/entry.cpp"
