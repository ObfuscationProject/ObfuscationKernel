#pragma once

// Runtime behavior is independent of diagnostics and build mode. Historical
// hosted fixtures explicitly selecting private diagnostics retain their runtime.
#if !defined(OK_NATIVE_SIGNAL_RUNTIME) && defined(OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE) && defined(OK_ENABLE_TEST_POINTS)
#define OK_NATIVE_SIGNAL_RUNTIME 1
#endif
