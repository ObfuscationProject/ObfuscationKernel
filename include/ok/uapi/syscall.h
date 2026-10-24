#ifndef OK_UAPI_SYSCALL_H
#define OK_UAPI_SYSCALL_H

#include <stdint.h>

#define OK_UAPI_VERSION_MAJOR 0
#define OK_UAPI_VERSION_MINOR 16
#define OK_UAPI_VERSION_PATCH 0

#define OK_UAPI_X86_64_SYSCALL 1
#define OK_UAPI_SYSCALL_ARG_COUNT 6
#define OK_NATIVE_CREDENTIALS_VERSION 1
#define OK_NATIVE_SESSIONS_VERSION 3
#define OK_NATIVE_CONSOLE_LEASE_VERSION 1
#define OK_NATIVE_CONSOLE_CONTROL_VERSION 1
#define OK_TIOCSCTTY 0x540eU
#define OK_TIOCGPGRP 0x540fU
#define OK_TIOCSPGRP 0x5410U
#define OK_TIOCGSID 0x5429U
#define OK_NATIVE_PERSISTENT_SYNC_VERSION 1
#define OK_NATIVE_POWER_VERSION 1
#define OK_NATIVE_METADATA_VERSION 1
#define OK_NATIVE_MOUNT_INFO_VERSION 1
#define OK_MOUNT_INFO_CAPACITY 9U
#define OK_MOUNT_INFO_PATH_CAPACITY 256U
#define OK_MOUNT_STATE_UNUSED 0U
#define OK_MOUNT_STATE_UNDERLYING_RAM 1U
#define OK_MOUNT_STATE_BORROWED 2U
#define OK_MOUNT_STATE_OWNED_PROVIDER 3U
#define OK_MOUNT_STATE_OWNED_RAM 4U
#define OK_MOUNT_STATE_DETACHED 5U
#define OK_CONSOLE_EPOCH_MAX 0x3fffffffU
#define OK_POWER_TOKEN_MAX 0x3fffffffU
#define OK_POWER_HALT 1U
#define OK_POWER_REBOOT 2U
#define OK_POWER_POWEROFF 3U

#define OK_SYS_READ 0ULL
#define OK_SYS_WRITE 1ULL
#define OK_SYS_OPEN 2ULL
#define OK_SYS_CLOSE 3ULL
#define OK_SYS_STAT 4ULL
#define OK_SYS_FSTAT 5ULL
#define OK_SYS_LSTAT 6ULL
#define OK_SYS_POLL 7ULL
#define OK_SYS_LSEEK 8ULL
#define OK_SYS_MMAP 9ULL
#define OK_SYS_MPROTECT 10ULL
#define OK_SYS_MUNMAP 11ULL
#define OK_SYS_BRK 12ULL
#define OK_SYS_RT_SIGACTION 13ULL
#define OK_SYS_RT_SIGPROCMASK 14ULL
#define OK_SYS_IOCTL 16ULL
#define OK_SYS_PREAD64 17ULL
#define OK_SYS_PWRITE64 18ULL
#define OK_SYS_READV 19ULL
#define OK_SYS_WRITEV 20ULL
#define OK_SYS_ACCESS 21ULL
#define OK_SYS_PIPE 22ULL
#define OK_SYS_SELECT 23ULL
#define OK_SYS_SCHED_YIELD 24ULL
#define OK_SYS_DUP 32ULL
#define OK_SYS_DUP2 33ULL
#define OK_SYS_NANOSLEEP 35ULL
#define OK_SYS_GETPID 39ULL
#define OK_SYS_SOCKET 41ULL
#define OK_SYS_CONNECT 42ULL
#define OK_SYS_ACCEPT 43ULL
#define OK_SYS_SENDTO 44ULL
#define OK_SYS_RECVFROM 45ULL
#define OK_SYS_SENDMSG 46ULL
#define OK_SYS_RECVMSG 47ULL
#define OK_SYS_SHUTDOWN 48ULL
#define OK_SYS_BIND 49ULL
#define OK_SYS_LISTEN 50ULL
#define OK_SYS_GETSOCKNAME 51ULL
#define OK_SYS_GETPEERNAME 52ULL
#define OK_SYS_SOCKETPAIR 53ULL
#define OK_SYS_GETSOCKOPT 55ULL
#define OK_SYS_CLONE 56ULL
#define OK_SYS_FORK 57ULL
#define OK_SYS_EXECVE 59ULL
#define OK_SYS_EXIT 60ULL
#define OK_SYS_WAIT4 61ULL
#define OK_SYS_KILL 62ULL
#define OK_SYS_UNAME 63ULL
#define OK_SYS_FCNTL 72ULL
#define OK_SYS_FSYNC 74ULL
#define OK_SYS_FDATASYNC 75ULL
#define OK_SYS_TRUNCATE 76ULL
#define OK_SYS_FTRUNCATE 77ULL
#define OK_SYS_GETDENTS 78ULL
#define OK_SYS_GETCWD 79ULL
#define OK_SYS_CHDIR 80ULL
#define OK_SYS_FCHDIR 81ULL
#define OK_SYS_RENAME 82ULL
#define OK_SYS_MKDIR 83ULL
#define OK_SYS_RMDIR 84ULL
#define OK_SYS_CREAT 85ULL
#define OK_SYS_LINK 86ULL
#define OK_SYS_UNLINK 87ULL
#define OK_SYS_SYMLINK 88ULL
#define OK_SYS_READLINK 89ULL
#define OK_SYS_CHMOD 90ULL
#define OK_SYS_FCHMOD 91ULL
#define OK_SYS_CHOWN 92ULL
#define OK_SYS_FCHOWN 93ULL
#define OK_SYS_UMASK 95ULL
#define OK_SYS_GETTIMEOFDAY 96ULL
#define OK_SYS_GETRLIMIT 97ULL
#define OK_SYS_SYSINFO 99ULL
#define OK_SYS_GETUID 102ULL
#define OK_SYS_GETGID 104ULL
#define OK_SYS_SETUID 105ULL
#define OK_SYS_SETGID 106ULL
#define OK_SYS_GETEUID 107ULL
#define OK_SYS_GETEGID 108ULL
#define OK_SYS_SETPGID 109ULL
#define OK_SYS_GETPPID 110ULL
#define OK_SYS_GETPGRP 111ULL
#define OK_SYS_SETSID 112ULL
#define OK_SYS_SETREUID 113ULL
#define OK_SYS_SETREGID 114ULL
#define OK_SYS_SETRESUID 117ULL
#define OK_SYS_GETRESUID 118ULL
#define OK_SYS_SETRESGID 119ULL
#define OK_SYS_GETRESGID 120ULL
#define OK_SYS_GETPGID 121ULL
#define OK_SYS_GETSID 124ULL
#define OK_SYS_ARCH_PRCTL 158ULL
#define OK_SYS_GETTID 186ULL
#define OK_SYS_TIME 201ULL
#define OK_SYS_FUTEX 202ULL
#define OK_SYS_GETDENTS64 217ULL
#define OK_SYS_SET_TID_ADDRESS 218ULL
#define OK_SYS_CLOCK_SETTIME 227ULL
#define OK_SYS_CLOCK_GETTIME 228ULL
#define OK_SYS_CLOCK_GETRES 229ULL
#define OK_SYS_CLOCK_NANOSLEEP 230ULL
#define OK_SYS_EXIT_GROUP 231ULL
#define OK_SYS_OPENAT 257ULL
#define OK_SYS_MKDIRAT 258ULL
#define OK_SYS_FCHOWNAT 260ULL
#define OK_SYS_NEWFSTATAT 262ULL
#define OK_SYS_UNLINKAT 263ULL
#define OK_SYS_RENAMEAT 264ULL
#define OK_SYS_LINKAT 265ULL
#define OK_SYS_SYMLINKAT 266ULL
#define OK_SYS_READLINKAT 267ULL
#define OK_SYS_FCHMODAT 268ULL
#define OK_SYS_FACCESSAT 269ULL
#define OK_SYS_SET_ROBUST_LIST 273ULL
#define OK_SYS_DUP3 292ULL
#define OK_SYS_ACCEPT4 288ULL
#define OK_SYS_PIPE2 293ULL
#define OK_SYS_PRLIMIT64 302ULL
#define OK_SYS_GETRANDOM 318ULL
#define OK_SYS_MEMFD_CREATE 319ULL
#define OK_SYS_RSEQ 334ULL
#define OK_SYS_CLOSE_RANGE 436ULL
#define OK_SYS_FACCESSAT2 439ULL
#define OK_SYS_OK_DEBUG 1024ULL
#define OK_SYS_LOAD_MODULE 1025ULL
#define OK_SYS_FSTAT_V2 1026ULL
#define OK_SYS_NEWFSTATAT_V2 1027ULL
#define OK_SYS_CONSOLE_CLAIM 1028ULL
#define OK_SYS_CONSOLE_REVOKE 1029ULL
#define OK_SYS_CONSOLE_FLUSH 1030ULL
#define OK_SYS_SYNC_PERSISTENT 1031ULL
/* Native-only, original PID1: a0 is action/token, a1..a5 must be zero.
 * PREPARE returns a positive boot-local selector; COMMIT only queues work. */
#define OK_SYS_POWER_PREPARE 1032ULL
#define OK_SYS_POWER_COMMIT 1033ULL
/* Native-only observation: (slot, output, sizeof(ok_mount_info), 0, 0, 0).
 * Slots and generation belong to one VFS lifetime. No mutation, capability,
 * reference, synchronization or shutdown admission is granted. */
#define OK_SYS_MOUNT_INFO 1034ULL
/* Native raw transport: (int32_t output[2], flags, 0, 0, 0, 0). */
#define OK_SYS_PTY_PAIR 1035ULL
#define OK_NATIVE_PTY_PAIR_VERSION 1U
#define OK_NATIVE_PTY_NONBLOCK 0x0800U
#define OK_NATIVE_PTY_CLOEXEC 0x080000U

// Native only. Argument 0 is a nonnegative fd; arguments 1..5 must be zero.
// Success is zero after the live retained provider completes its journal work
// and backing-device FLUSH. Volatile/unqualified providers return EOPNOTSUPP.

// Native only. Original PID 1 claims a new direct user child, returning a
// positive bounded epoch. Revoke requires PID 1 and that expected epoch; flush
// permits PID 1 or the active leader. Unused arguments must be zero. Input is
// default-closed, never automatically echoed, and lost on revoke/leader death.
// A claim or flush with nonquiescent serial ingress fails closed with EIO.

// Credential setters consume the low 32 bits of each ID argument. All-ones
// means unchanged for setre*/setres* and is invalid for setuid/setgid. Setters
// require exactly one live thread; otherwise they return -OK_EOPNOTSUPP.
// getres* stores three uint32_t IDs in target byte order, left to right. An
// -OK_EFAULT preserves stores to earlier pointers. IDs carry no kernel access.
#define OK_ID_UNCHANGED UINT32_C(0xffffffff)

// Session and process-group IDs share the positive process-ID domain.
// PID arguments consume the low signed 32 bits on every architecture. Zero
// selects the caller for getpgid/getsid/setpgid and the target PID for setpgid's
// PGID. Fork inherits SID/PGID; successful exec and reparenting preserve them.
// These operations establish process membership only. They do not acquire a
// controlling terminal or provide PTYs, job control or group signal delivery.

#define OK_MFD_CLOEXEC 1U
#define OK_MFD_ALLOW_SEALING 2U
#define OK_F_ADD_SEALS 1033U
#define OK_F_GET_SEALS 1034U
#define OK_F_SEAL_SEAL 1U
#define OK_F_SEAL_SHRINK 2U
#define OK_F_SEAL_GROW 4U
#define OK_F_SEAL_WRITE 8U

#define OK_EPERM 1
#define OK_ENOENT 2
#define OK_ESRCH 3
#define OK_EIO 5
#define OK_EBADF 9
#define OK_EAGAIN 11
#define OK_ENOMEM 12
#define OK_EACCES 13
#define OK_EFAULT 14
#define OK_EEXIST 17
#define OK_EINVAL 22
#define OK_ENOTTY 25
#define OK_EPIPE 32
#define OK_ENOSYS 38
#define OK_ELOOP 40
#define OK_EOVERFLOW 75
#define OK_ENOTSOCK 88
#define OK_EMSGSIZE 90
#define OK_EOPNOTSUPP 95
#define OK_EAFNOSUPPORT 97
#define OK_EADDRINUSE 98
#define OK_ENETUNREACH 101
#define OK_ECONNABORTED 103
#define OK_ECONNRESET 104
#define OK_ENOBUFS 105
#define OK_EISCONN 106
#define OK_ENOTCONN 107
#define OK_ETIMEDOUT 110
#define OK_ECONNREFUSED 111
#define OK_EALREADY 114
#define OK_EINPROGRESS 115

// Native diagnostic sockaddr_in uses native family and network-order port/IP
// bytes. It is deliberately not an alias of any Linux personality structure.
#define OK_AF_INET 2U
#define OK_AF_UNIX 1U
#define OK_SOCK_STREAM 1U
#define OK_SOCK_DGRAM 2U
#define OK_SOCK_NONBLOCK 0x0800U
#define OK_SOCK_CLOEXEC 0x080000U
#define OK_IPPROTO_UDP 17U
#define OK_IPPROTO_TCP 6U
#define OK_SHUT_WR 1U
#define OK_SOL_SOCKET 1U
#define OK_SCM_RIGHTS 1U
#define OK_MSG_PEEK 0x0002U
#define OK_MSG_CTRUNC 0x0008U
#define OK_MSG_DONTWAIT 0x0040U
#define OK_MSG_NOSIGNAL 0x4000U
#define OK_MSG_CMSG_CLOEXEC 0x40000000U
#define OK_SO_ERROR 4U
#define OK_SO_TYPE 3U
#define OK_SO_PEERCRED 17U
#define OK_UNIX_PATH_CAPACITY 108U

struct ok_sockaddr_un
{
    uint16_t family;
    char path[OK_UNIX_PATH_CAPACITY];
};
struct ok_ucred
{
    int32_t pid;
    uint32_t uid;
    uint32_t gid;
};

struct ok_sockaddr_in
{
    uint16_t family;
    uint8_t port_be[2];
    uint8_t address[4];
    uint8_t reserved[8];
};
typedef uint32_t ok_socklen_t; // Native accept address capacity and full output size.

#define OK_O_RDONLY 0x0000U
#define OK_O_WRONLY 0x0001U
#define OK_O_RDWR 0x0002U
#define OK_O_APPEND 0x0008U
#define OK_O_CREAT 0x0040U
#define OK_O_EXCL 0x0080U
#define OK_O_TRUNC 0x0200U
#define OK_O_NONBLOCK 0x0800U
#define OK_O_CLOEXEC 0x080000U
#define OK_O_DIRECTORY 0x0200000U
#define OK_O_NOFOLLOW 0x020000U

#define OK_AT_FDCWD (-100)
#define OK_AT_SYMLINK_NOFOLLOW 0x0100U
#define OK_AT_EMPTY_PATH 0x1000U

#define OK_R_OK 4U
#define OK_W_OK 2U
#define OK_X_OK 1U
#define OK_F_OK 0U

#define OK_PROT_NONE 0x0U
#define OK_PROT_READ 0x1U
#define OK_PROT_WRITE 0x2U

#define OK_MAP_SHARED 0x01U
#define OK_MAP_PRIVATE 0x02U
#define OK_MAP_FIXED 0x10U
#define OK_MAP_ANONYMOUS 0x20U

#define OK_F_DUPFD 0U
#define OK_F_GETFD 1U
#define OK_F_SETFD 2U
#define OK_F_GETFL 3U
#define OK_F_SETFL 4U
#define OK_FD_CLOEXEC 1U

#define OK_SEEK_SET 0U
#define OK_SEEK_CUR 1U
#define OK_SEEK_END 2U

#define OK_ARCH_SET_GS 0x1001U
#define OK_ARCH_SET_FS 0x1002U
#define OK_ARCH_GET_FS 0x1003U
#define OK_ARCH_GET_GS 0x1004U

#define OK_FUTEX_WAIT 0U
#define OK_FUTEX_WAKE 1U

#define OK_CLOCK_REALTIME 0U
#define OK_CLOCK_MONOTONIC 1U
#define OK_TIMER_ABSTIME 1U

#define OK_POLLIN 0x0001U
#define OK_POLLOUT 0x0004U
#define OK_POLLERR 0x0008U
#define OK_POLLHUP 0x0010U
#define OK_POLLNVAL 0x0020U

/* Fixed-width fields use target native byte order. All 288 output bytes,
 * including the NUL terminator and unused path suffix, are initialized. */
struct ok_mount_info
{
    uint32_t version;
    uint32_t size;
    uint32_t generation;
    uint32_t slot;
    uint32_t capacity;
    uint32_t state;
    uint32_t parent_id;
    uint32_t path_length;
    char path[OK_MOUNT_INFO_PATH_CAPACITY];
};

struct ok_pollfd
{
    int32_t fd;
    int16_t events;
    int16_t revents;
};

#define OK_NODE_DIRECTORY 0U
#define OK_NODE_REGULAR 1U
#define OK_NODE_DEVICE 2U
#define OK_NODE_SYMLINK 3U

#define OK_MODE_TYPE_MASK 0170000U
#define OK_MODE_SYMLINK 0120000U
#define OK_MODE_REGULAR 0100000U
#define OK_MODE_BLOCK_DEVICE 0060000U
#define OK_MODE_DIRECTORY 0040000U
#define OK_MODE_CHARACTER_DEVICE 0020000U
#define OK_MODE_FIFO 0010000U
#define OK_MODE_PERMISSION_MASK 07777U

struct ok_timespec
{
    int64_t seconds;
    int64_t nanoseconds;
};

struct ok_timeval
{
    int64_t seconds;
    int64_t microseconds;
};

struct ok_iovec
{
    uintptr_t base;
    uintptr_t length;
};

// Native target-word layouts. These types are not host/Linux struct aliases.
struct ok_msghdr
{
    uintptr_t name;
    uint32_t name_length;
    uintptr_t vectors;
    uintptr_t vector_count;
    uintptr_t control;
    uintptr_t control_length;
    uint32_t flags;
};
struct ok_cmsghdr
{
    uintptr_t length;
    int32_t level;
    int32_t type;
};
#define OK_CMSG_ALIGN(length) (((length) + sizeof(uintptr_t) - 1) & ~(sizeof(uintptr_t) - 1))
#define OK_CMSG_LEN(length) (sizeof(struct ok_cmsghdr) + (length))
#define OK_CMSG_SPACE(length) (sizeof(struct ok_cmsghdr) + OK_CMSG_ALIGN(length))

struct ok_stat
{
    uint8_t type;
    uintptr_t size;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t link_count;
    uint32_t block_size;
    uint64_t blocks;
};

// Additive native extension; legacy stat calls still write only ok_stat.
struct ok_stat_v2
{
    struct ok_stat basic;
    uint64_t inode;
};

struct ok_rlimit
{
    uint64_t current;
    uint64_t maximum;
};

struct ok_sysinfo
{
    int64_t uptime;
    uint64_t total_ram;
    uint64_t free_ram;
    uint16_t process_count;
};

#endif
