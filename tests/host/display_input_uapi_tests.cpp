// Layout contract for the draft display/input/seat UAPI: fixed sizes on every
// profile, no padding surprises. Compiled with the host compiler for LP64 and
// with -m32 where available for ILP32.
#include "ok/uapi/display.h"
#include "ok/uapi/input.h"

#include <cstddef>
#include <cstdio>

static_assert(sizeof(struct ok_display_mode) == 32);
static_assert(sizeof(struct ok_display_info) == 32 + OK_DISPLAY_MAX_MODES * 32);
static_assert(sizeof(struct ok_display_buffer_request) == 48);
static_assert(offsetof(struct ok_display_buffer_request, length) == 32);
static_assert(sizeof(struct ok_display_present) == 24);
static_assert(sizeof(struct ok_display_event) == 32);
static_assert(sizeof(struct ok_input_event) == 16);
static_assert(offsetof(struct ok_input_event, value) == 12);
static_assert(sizeof(struct ok_input_device_info) == 32 + OK_INPUT_DEVICE_NAME_CAPACITY);
static_assert(sizeof(struct ok_seat_lease) == 24);
static_assert(OK_DISPLAY_UAPI_VERSION == 0 && OK_INPUT_UAPI_VERSION == 0 && OK_SEAT_UAPI_VERSION == 0,
              "draft headers; bump together with the first native handler");

int main()
{
    std::puts("OK_HOST display_input_uapi layout=pass");
    return 0;
}
