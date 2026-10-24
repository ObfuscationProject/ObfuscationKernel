#ifndef OK_UAPI_INPUT_H
#define OK_UAPI_INPUT_H

/*
 * Input UAPI, draft 1 (feat/display-input-uapi). Events are fixed-width records
 * read from /dev/input/eventN by the seat lease holder; the kernel keeps a
 * bounded per-device queue and reports drops. The layout follows the Linux
 * evdev convention (type/code/value) so a Wayland compositor can reuse its
 * decoding tables, but the type/code numbering is this project's UAPI. No kernel
 * handler produces these records yet; OK_INPUT_UAPI_VERSION stays 0 until then.
 */

#include <stdint.h>

#define OK_INPUT_UAPI_VERSION 0

#define OK_INPUT_TYPE_SYN 0U
#define OK_INPUT_TYPE_KEY 1U
#define OK_INPUT_TYPE_REL 2U
#define OK_INPUT_TYPE_ABS 3U

#define OK_INPUT_REL_X 0U
#define OK_INPUT_REL_Y 1U
#define OK_INPUT_REL_WHEEL 8U

#define OK_INPUT_DEVICE_NAME_CAPACITY 32U

struct ok_input_event {
    uint64_t timestamp_ns; /* CLOCK_MONOTONIC */
    uint16_t type;         /* OK_INPUT_TYPE_* */
    uint16_t code;         /* key code or axis */
    int32_t value;         /* 0/1/2 for keys, delta for REL, position for ABS */
};

struct ok_input_device_info {
    uint32_t size;         /* sizeof(struct ok_input_device_info) */
    uint32_t version;      /* OK_INPUT_UAPI_VERSION */
    uint32_t capabilities; /* bit per OK_INPUT_TYPE_* */
    uint32_t queue_capacity;
    uint32_t dropped;      /* events dropped since open */
    uint32_t reserved[3];
    char name[OK_INPUT_DEVICE_NAME_CAPACITY];
};

#define OK_INPUT_IOC_GET_INFO 0x6f490001U

/* Seat leases: one holder owns display scanout and input delivery at a time. */
#define OK_SEAT_UAPI_VERSION 0

struct ok_seat_lease {
    uint32_t size;        /* sizeof(struct ok_seat_lease) */
    uint32_t version;     /* OK_SEAT_UAPI_VERSION */
    uint32_t seat;        /* 0 = seat0 */
    uint32_t flags;       /* reserved, must be 0 */
    uint64_t generation;  /* out: lease generation; revoked leases stop matching */
};

#define OK_SEAT_IOC_ACQUIRE 0x6f530001U
#define OK_SEAT_IOC_RELEASE 0x6f530002U

#endif
