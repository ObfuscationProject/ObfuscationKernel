#ifndef OK_UAPI_DISPLAY_H
#define OK_UAPI_DISPLAY_H

/*
 * Display UAPI, draft 1 (feat/display-input-uapi). Fixed-width records with a
 * size/version prefix; no pointers, no C++ types. A user-space compositor opens
 * /dev/display0, queries modes, allocates dumb buffers, maps them with mmap and
 * presents them. Scanout and buffer ownership belong to the seat lease holder.
 * No kernel handler accepts these requests yet; OK_DISPLAY_UAPI_VERSION stays 0
 * until the first native implementation lands.
 */

#include <stdint.h>

#define OK_DISPLAY_UAPI_VERSION 0

#define OK_DISPLAY_FORMAT_XRGB8888 1U
#define OK_DISPLAY_FORMAT_ARGB8888 2U

#define OK_DISPLAY_MAX_MODES 8U
#define OK_DISPLAY_MAX_BUFFERS 16U

struct ok_display_mode {
    uint32_t size;        /* sizeof(struct ok_display_mode) */
    uint32_t width;
    uint32_t height;
    uint32_t refresh_mhz; /* 60000 = 60 Hz; 0 when the backend has no vblank */
    uint32_t format;      /* OK_DISPLAY_FORMAT_* */
    uint32_t reserved[3];
};

struct ok_display_info {
    uint32_t size;        /* sizeof(struct ok_display_info) */
    uint32_t version;     /* OK_DISPLAY_UAPI_VERSION */
    uint32_t mode_count;
    uint32_t active_mode;
    uint32_t max_buffers;
    uint32_t reserved[3];
    struct ok_display_mode modes[OK_DISPLAY_MAX_MODES];
};

struct ok_display_buffer_request {
    uint32_t size;        /* sizeof(struct ok_display_buffer_request) */
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t flags;       /* reserved, must be 0 */
    uint32_t handle;      /* out: buffer handle, nonzero */
    uint32_t pitch;       /* out: bytes per row */
    uint32_t reserved;
    uint64_t length;      /* out: mmap length in bytes */
    uint64_t offset;      /* out: mmap offset on the display descriptor */
};

struct ok_display_present {
    uint32_t size;        /* sizeof(struct ok_display_present) */
    uint32_t handle;      /* buffer to scan out */
    uint32_t flags;       /* OK_DISPLAY_PRESENT_* */
    uint32_t reserved;
    uint64_t sequence;    /* out: vblank sequence the flip completes on */
};

#define OK_DISPLAY_PRESENT_WAIT_VBLANK 1U

#define OK_DISPLAY_EVENT_VBLANK 1U
#define OK_DISPLAY_EVENT_LEASE_REVOKED 2U
#define OK_DISPLAY_EVENT_MODE_CHANGED 3U

struct ok_display_event {
    uint32_t size;        /* sizeof(struct ok_display_event) */
    uint32_t type;        /* OK_DISPLAY_EVENT_* */
    uint64_t sequence;
    uint64_t timestamp_ns; /* CLOCK_MONOTONIC */
    uint32_t data;
    uint32_t reserved;
};

/* ioctl numbers reserved in the 'oD' group; the handlers do not exist yet. */
#define OK_DISPLAY_IOC_GET_INFO      0x6f440001U
#define OK_DISPLAY_IOC_CREATE_BUFFER 0x6f440002U
#define OK_DISPLAY_IOC_DESTROY_BUFFER 0x6f440003U
#define OK_DISPLAY_IOC_PRESENT       0x6f440004U
#define OK_DISPLAY_IOC_SET_MODE      0x6f440005U

#endif
