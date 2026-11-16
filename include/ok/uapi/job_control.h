#ifndef OK_UAPI_JOB_CONTROL_H
#define OK_UAPI_JOB_CONTROL_H

#include <stdint.h>

/* Native job-control signals. The numbers are the same on every profile and
 * equal the Linux generic numbering; OK_SIGKILL, OK_SIGTERM and
 * OK_NATIVE_SIGNAL_MAX stay in ok/uapi/signal.h. */
#define OK_SIGHUP 1
#define OK_SIGINT 2
#define OK_SIGQUIT 3
#define OK_SIGCHLD 17
#define OK_SIGCONT 18
#define OK_SIGSTOP 19
#define OK_SIGTSTP 20
#define OK_SIGTTIN 21
#define OK_SIGTTOU 22
#define OK_SIGWINCH 28

/* Native rt_sigaction(sig, act, oact, 8) accepts these two handlers only;
 * flags and mask must be zero. SIG_IGN for OK_SIGCHLD is refused. */
#define OK_SIG_DFL 0
#define OK_SIG_IGN 1
#define OK_NATIVE_SIGNAL_ACTION_VERSION 1

/* Native byte order, 24 bytes. */
struct ok_sigaction
{
    uint64_t handler;
    uint64_t flags;
    uint64_t mask;
};

/* Valid C and C++ size check: the array size is -1 when the layout drifts. */
typedef char ok_sigaction_size_check[sizeof(struct ok_sigaction) == 24 ? 1 : -1];

/* Native wait4 options; any other bit is EINVAL. */
#define OK_WNOHANG 1
#define OK_WUNTRACED 2
#define OK_WCONTINUED 8

#endif
