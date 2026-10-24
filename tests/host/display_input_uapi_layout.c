/* C layout contract for the draft display/input/seat UAPI. Compiled freestanding
 * with -fsyntax-only for LP64 and ILP32 so the sizes hold on every profile. */
#include "ok/uapi/display.h"
#include "ok/uapi/input.h"
#include <stddef.h>

_Static_assert(sizeof(struct ok_display_mode) == 32, "display mode");
_Static_assert(sizeof(struct ok_display_info) == 32 + OK_DISPLAY_MAX_MODES * 32, "display info");
_Static_assert(sizeof(struct ok_display_buffer_request) == 48, "buffer request");
_Static_assert(offsetof(struct ok_display_buffer_request, length) == 32, "buffer request length offset");
_Static_assert(sizeof(struct ok_display_present) == 24, "present");
_Static_assert(sizeof(struct ok_display_event) == 32, "display event");
_Static_assert(sizeof(struct ok_input_event) == 16, "input event");
_Static_assert(offsetof(struct ok_input_event, value) == 12, "input event value offset");
_Static_assert(sizeof(struct ok_input_device_info) == 32 + OK_INPUT_DEVICE_NAME_CAPACITY, "input device info");
_Static_assert(sizeof(struct ok_seat_lease) == 24, "seat lease");
