#!/usr/bin/env python3
"""Check producer capability and pre-publication mode/assembly consistency."""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import tempfile
from unittest.mock import patch
import shutil
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('export_sdk', ROOT / 'tools/export_sdk.py')
export_sdk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(export_sdk)
RUNTIME = 'OK_NATIVE_SIGNAL_RUNTIME'
PRIVATE = 'OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE'
TESTS = 'OK_ENABLE_TEST_POINTS'
ARCHES = ('i386', 'x86_64', 'aarch64', 'arm32', 'rv64', 'rv32', 'loongarch64', 'mips', 'mips64', 'ppc')
BOOT_ARCHIVE_DIRECTORY_HEADER = ('#pragma once\n#include "ok/core/types.hpp"\n#include <string_view>\n'
    'namespace ok::fs\n{\n'
    'inline constexpr std::string_view boot_archive_directory_profile = "root-owned-ram-exact-sticky-sgid-v1";\n'
    'inline constexpr u32 boot_archive_directory_mode_mask = 03777;\n'
    'inline constexpr u32 boot_archive_regular_mode_mask = 0777;\n} // namespace ok::fs\n')

CONSOLE_INPUT_HEADER = ('#pragma once\n#include "ok/core/types.hpp"\n#include <string_view>\n'
    'namespace ok::sched\n{\n'
    'inline constexpr std::string_view native_console_input_profile = "pid1-issued-epoch-fork-exec-leader-revoke-noecho-v1";\n'
    'inline constexpr u32 native_console_epoch_limit = 0x3fffffff;\n} // namespace ok::sched\n')
CONSOLE_INPUT_DEFINITIONS = (
    ('OK_NATIVE_CONSOLE_LEASE_VERSION', '1'), ('OK_CONSOLE_EPOCH_MAX', '0x3fffffffU'),
    ('OK_SYS_CONSOLE_CLAIM', '1028ULL'), ('OK_SYS_CONSOLE_REVOKE', '1029ULL'),
    ('OK_SYS_CONSOLE_FLUSH', '1030ULL'), ('OK_SYS_SYNC_PERSISTENT', '1031ULL'), ('OK_NATIVE_PERSISTENT_SYNC_VERSION', '1'))

POWER_HEADER = ('#pragma once\n#include "ok/core/types.hpp"\n#include <string_view>\n'
    'namespace ok::user\n{\n'
    'inline constexpr std::string_view native_power_profile = "original-init-scalar-prepare-queued-commit-v1";\n'
    'inline constexpr u32 native_power_version = 1;\n'
    'inline constexpr u32 native_power_token_limit = 0x3fffffff;\n} // namespace ok::user\n')
POWER_DEFINITIONS = (
    ('OK_NATIVE_POWER_VERSION', '1'), ('OK_POWER_TOKEN_MAX', '0x3fffffffU'),
    ('OK_POWER_HALT', '1U'), ('OK_POWER_REBOOT', '2U'), ('OK_POWER_POWEROFF', '3U'),
    ('OK_SYS_POWER_PREPARE', '1032ULL'), ('OK_SYS_POWER_COMMIT', '1033ULL'))

CREDENTIALS_PROFILE = 'bootcpu-single-live-thread-real-effective-saved-v1'
CREDENTIALS_HEADERS = ('ok/uapi/syscall.h', 'ok/user/native_credentials.hpp',
                       'ok/user/credential_change.hpp', 'ok/user/linux_credentials.hpp',
                       'ok/sched/scheduler.hpp')
CREDENTIALS_DEFINITIONS = (
    ('OK_NATIVE_CREDENTIALS_VERSION', '1'), ('OK_SYS_SETUID', '105ULL'),
    ('OK_SYS_SETGID', '106ULL'), ('OK_SYS_SETREUID', '113ULL'),
    ('OK_SYS_SETREGID', '114ULL'), ('OK_SYS_SETRESUID', '117ULL'),
    ('OK_SYS_GETRESUID', '118ULL'), ('OK_SYS_SETRESGID', '119ULL'),
    ('OK_SYS_GETRESGID', '120ULL'), ('OK_ID_UNCHANGED', 'UINT32_C(0xffffffff)'))
UAPI_HEADER = ('#ifndef OK_UAPI_SYSCALL_H\n#define OK_UAPI_SYSCALL_H\n#include <stdint.h>\n'
               '#define OK_UAPI_VERSION_MAJOR 0\n#define OK_UAPI_VERSION_MINOR 16\n'
               '#define OK_UAPI_VERSION_PATCH 0\n' +
               ''.join('#define ' + name + ' ' + value + '\n'
                       for name, value in CREDENTIALS_DEFINITIONS + CONSOLE_INPUT_DEFINITIONS + POWER_DEFINITIONS) + '#endif\n')
TERMINATION_HEADER = '#ifndef OK_UAPI_SIGNAL_H\n#define OK_UAPI_SIGNAL_H\n#define OK_SIGKILL 9\n#define OK_SIGTERM 15\n#define OK_NATIVE_SIGNAL_MAX 64\n#define OK_NATIVE_TERMINATION_VERSION 1\n#endif\n'
COMMON_HEADER = 'inline constexpr std::string_view ext4_common_opening_profile = "serialized-common-eight-family-directory-create-ordered-free-init-zero-refill-optional-revoke-legacy-v4";\ninline constexpr u32 ext4_common_module_abi = 75;\ninline constexpr u32 ext4_common_session_counter_bits = 64;\n'


UDP_HEADER = 'inline constexpr std::string_view native_udp_profile = "bootcpu-static-peer-ipv4-polling-captured-connected-zero-send-v3";\ninline constexpr std::string_view linux_udp_profile = "bootcpu-static-peer-ipv4-captured-connected-zero-send-v3";\ninline constexpr std::string_view native_udp_connected_profile = "bounded-static-peer-association-zero-destination-output-v2";\ninline constexpr u32 native_udp_module_abi = 75;\ninline constexpr usize native_udp_socket_capacity = 16;\ninline constexpr usize native_udp_queue_capacity = 2;\ninline constexpr usize native_udp_datagram_capacity = 1472;\ninline constexpr usize native_udp_frame_capacity = 1514;\n'


TCP_HEADER = 'inline constexpr std::string_view native_tcp_profile = "bootcpu-static-peer-ipv4-captured-nonblocking-immediate-v1";\ninline constexpr std::string_view linux_tcp_profile = "bootcpu-direct-static-peer-ipv4-captured-nonblocking-immediate-v1";\ninline constexpr std::string_view native_tcp_accept_profile = "bootcpu-explicit-ipv4-captured-nonblocking-reserved-accept-v1";\ninline constexpr std::string_view linux_tcp_accept_profile = "bootcpu-direct-explicit-ipv4-captured-nonblocking-reserved-accept-v1";\ninline constexpr u32 native_tcp_module_abi = 75;\ninline constexpr usize native_tcp_stream_capacity = 16;\ninline constexpr usize native_tcp_connection_capacity = 8;\ninline constexpr usize native_tcp_listener_capacity = 4;\ninline constexpr usize native_tcp_backlog_capacity = 4;\ninline constexpr usize native_tcp_send_capacity = 1024;\ninline constexpr usize native_tcp_receive_capacity = 1024;\ninline constexpr usize native_tcp_segment_capacity = 536;\ninline constexpr usize native_tcp_poll_budget = 8;\n'


SHARED_FILE_HEADER = 'inline constexpr std::string_view native_shared_file_profile = "bootcpu-volatile-sealed-size-write-eager-pages-v3";\ninline constexpr std::string_view linux_shared_file_profile = "bootcpu-volatile-sealed-size-write-eager-pages-v3";\ninline constexpr u32 native_shared_file_module_abi = 75;\ninline constexpr usize native_shared_file_page_size = 4096;\ninline constexpr u32 native_shared_file_supported_seals = 15;\n'

UNIX_PATHNAME_HEADER = (
    'inline constexpr std::string_view native_unix_pathname_profile = "bootcpu-ram-pathname-stream-owned-queue-direct-v1";\n'
    'inline constexpr std::string_view linux_unix_pathname_profile = "bootcpu-ram-pathname-stream-owned-queue-direct-v1";\n'
    'inline constexpr u32 unix_pathname_module_abi = 75;\n'
    'inline constexpr usize unix_pathname_endpoint_capacity = 32;\n'
    'inline constexpr usize unix_pathname_backlog_capacity = 4;\n'
    'inline constexpr usize unix_pathname_address_capacity = 110;\n'
    'inline constexpr usize unix_pathname_text_capacity = 107;\n'
    'inline constexpr usize unix_pathname_peer_credentials_bytes = 12;\n'
)

DIRECTORY_CREATE_HEADER = (
    'inline constexpr std::string_view ext4_directory_create_profile = "bounded-journalled-single-block-directory-create-ordered-free-init-v2";\n'
    'inline constexpr u32 ext4_directory_create_module_abi = 75;\n'
    'inline constexpr usize ext4_directory_create_home_capacity = 8;\n'
    'inline constexpr usize ext4_directory_create_owner_buffers = 9;\n'
)

FUTEX_WAKE_OPERATION_HEADER = 'inline constexpr std::string_view native_futex_wake_operation_profile = "bootcpu-serialized-two-key-private-anonymous-shared-signed-wrap-legacy-count-v1";\ninline constexpr std::string_view linux_futex_wake_operation_profile = "bootcpu-serialized-two-key-private-anonymous-shared-signed-wrap-legacy-count-v1";\ninline constexpr u32 native_futex_wake_operation_module_abi = 75;\ninline constexpr usize native_futex_wake_operation_word_bits = 32;\ninline constexpr usize native_futex_wake_operation_operand_bits = 12;\ninline constexpr usize native_futex_wake_operation_shift_bits = 5;\ninline constexpr usize native_futex_wake_operation_arithmetic_count = 5;\ninline constexpr usize native_futex_wake_operation_comparison_count = 6;\ninline constexpr usize native_futex_wake_operation_wake_passes = 2;\ninline constexpr usize native_futex_wake_operation_key_domains = 2;\ninline constexpr usize native_futex_wake_operation_nonpositive_wake_minimum = 1;\ninline constexpr u32 native_futex_wake_operation_smp_atomic = 0;\ninline constexpr u32 native_futex_wake_operation_file_backed = 0;\ninline constexpr u32 native_futex_wake_operation_cross_process_clone_vm = 0;\n'


FUTEX_REQUEUE_HEADER = 'inline constexpr std::string_view native_futex_requeue_profile = "bootcpu-nonpi-read-keys-signed-counts-exact-origin-ticket-sequence-owned-transfer-v1";\ninline constexpr std::string_view linux_futex_requeue_profile = "bootcpu-nonpi-read-keys-signed-counts-exact-origin-ticket-sequence-owned-transfer-v1";\ninline constexpr u32 native_futex_requeue_module_abi = 75;\ninline constexpr usize native_futex_requeue_word_bits = 32;\ninline constexpr usize native_futex_requeue_operation_count = 2;\ninline constexpr usize native_futex_requeue_linux_operation = 3;\ninline constexpr usize native_futex_cmp_requeue_linux_operation = 4;\ninline constexpr usize native_futex_requeue_private_flag = 128;\ninline constexpr usize native_futex_requeue_key_domains = 2;\ninline constexpr usize native_futex_requeue_waiter_capacity = 64;\ninline constexpr usize native_futex_requeue_sequence_bits = 64;\ninline constexpr u32 native_futex_requeue_original_continuation_preserved = 1;\ninline constexpr u32 native_futex_requeue_same_key_sequence_preserved = 1;\ninline constexpr u32 native_futex_requeue_zero_wake_minimum = 0;\ninline constexpr u32 native_futex_requeue_signed_negative_counts_rejected = 1;\ninline constexpr u32 native_futex_requeue_transfer_pin_owners_retained = 1;\ninline constexpr u32 native_futex_requeue_smp_atomic = 0;\ninline constexpr u32 native_futex_requeue_priority_inheritance = 0;\ninline constexpr u32 native_futex_requeue_realtime_priority = 0;\ninline constexpr u32 native_futex_requeue_file_backed = 0;\ninline constexpr u32 native_futex_requeue_cross_process_clone_vm = 0;\n'

METADATA_HEADER = '#pragma once\n#include "ok/core/types.hpp"\n#include <string_view>\nnamespace ok::user\n{\ninline constexpr std::string_view native_metadata_profile = "checked-mode-root-owner-u32-v1";\ninline constexpr u32 native_metadata_version = 1;\ninline constexpr u32 native_metadata_module_abi = 75;\n} // namespace ok::user\n'
METADATA_DEFINITIONS = (('OK_NATIVE_METADATA_VERSION', '1'), ('OK_SYS_CHMOD', '90ULL'), ('OK_SYS_FCHMOD', '91ULL'), ('OK_SYS_CHOWN', '92ULL'), ('OK_SYS_FCHOWN', '93ULL'), ('OK_SYS_FCHOWNAT', '260ULL'), ('OK_SYS_FCHMODAT', '268ULL'), ('OK_AT_EMPTY_PATH', '0x1000U'), ('OK_AT_SYMLINK_NOFOLLOW', '0x0100U'))
UAPI_HEADER = UAPI_HEADER.replace("#endif", "".join("#define " + n + " " + v + "\n" for n,v in METADATA_DEFINITIONS) + "#endif")

SESSIONS_HEADER = '#pragma once\n#include "ok/core/types.hpp"\n#include <string_view>\nnamespace ok::user\n{\ninline constexpr std::string_view native_sessions_profile = "bootcpu-serialized-process-group-session-console-lifecycle-v3";\ninline constexpr u32 native_sessions_version = 3;\ninline constexpr u32 native_sessions_module_abi = 75;\ninline constexpr u32 native_sessions_pid_bits = 32;\ninline constexpr u32 native_sessions_process_capacity = 64;\ninline constexpr u32 native_sessions_fork_inherits = 1;\ninline constexpr u32 native_sessions_exec_preserves = 1;\ninline constexpr u32 native_sessions_reparent_preserves = 1;\ninline constexpr u32 native_sessions_controlling_tty = 1;\ninline constexpr u32 native_sessions_pty = 0;\ninline constexpr u32 native_sessions_job_control = 0;\n} // namespace ok::user\n'
GROUP_SIGNAL_HEADER = '#pragma once\n#include "ok/core/types.hpp"\n#include <string_view>\nnamespace ok::user\n{\ninline constexpr std::string_view native_group_signal_profile = "bootcpu-signed32-current-negative-pgid-probe-term-kill-deferred-partial-v2";\ninline constexpr u32 native_group_signal_version = 2;\ninline constexpr u32 native_group_signal_module_abi = 75;\ninline constexpr u32 native_group_signal_pid_bits = 32;\ninline constexpr u32 native_group_signal_process_capacity = 64;\ninline constexpr u32 native_group_signal_current_group = 1;\ninline constexpr u32 native_group_signal_negative_group = 1;\ninline constexpr u32 native_group_signal_probe = 1;\ninline constexpr u32 native_group_signal_term = 15;\ninline constexpr u32 native_group_signal_kill = 9;\ninline constexpr u32 native_group_signal_deferred_fatal = 1;\ninline constexpr u32 native_group_signal_partial_success = 1;\ninline constexpr u32 native_group_signal_broadcast = 0;\ninline constexpr u32 native_group_signal_stop_continue = 0;\ninline constexpr u32 native_group_signal_controlling_tty = 1;\ninline constexpr u32 native_group_signal_job_control = 0;\ninline constexpr u32 native_group_signal_smp = 0;\n} // namespace ok::user\n'
SESSIONS_DEFINITIONS = (("OK_NATIVE_SESSIONS_VERSION", "3"), ("OK_SYS_SETPGID", "109ULL"), ("OK_SYS_GETPGRP", "111ULL"), ("OK_SYS_SETSID", "112ULL"), ("OK_SYS_GETPGID", "121ULL"), ("OK_SYS_GETSID", "124ULL"), ("OK_ESRCH", "3"))
UAPI_HEADER = UAPI_HEADER.replace("#endif", "".join("#define " + n + " " + v + "\n" for n,v in SESSIONS_DEFINITIONS) + "#endif")

def write_pathname_and_directory_headers(headers):
    for name in CREDENTIALS_HEADERS:
        path = headers / name
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.exists():
            path.write_text(UAPI_HEADER if name == 'ok/uapi/syscall.h' else name)
    for names, marker, declaration in (
            (export_sdk.UNIX_PATHNAME_PROFILE_HEADERS, export_sdk.UNIX_PATHNAME_PROFILE_HEADER, UNIX_PATHNAME_HEADER),
            (export_sdk.DIRECTORY_CREATE_PROFILE_HEADERS, export_sdk.DIRECTORY_CREATE_PROFILE_HEADER, DIRECTORY_CREATE_HEADER),
            (export_sdk.FUTEX_WAKE_OPERATION_PROFILE_HEADERS, export_sdk.FUTEX_WAKE_OPERATION_PROFILE_HEADER, FUTEX_WAKE_OPERATION_HEADER),
            (export_sdk.FUTEX_REQUEUE_PROFILE_HEADERS, export_sdk.FUTEX_REQUEUE_PROFILE_HEADER, FUTEX_REQUEUE_HEADER),
            (export_sdk.BOOT_ARCHIVE_DIRECTORY_PROFILE_HEADERS, export_sdk.BOOT_ARCHIVE_DIRECTORY_PROFILE_HEADER, BOOT_ARCHIVE_DIRECTORY_HEADER),
            (export_sdk.NATIVE_CONSOLE_INPUT_HEADERS, export_sdk.NATIVE_CONSOLE_INPUT_PROFILE_HEADER, CONSOLE_INPUT_HEADER),
            (export_sdk.NATIVE_PERSISTENT_SYNC_HEADERS, export_sdk.NATIVE_PERSISTENT_SYNC_PROFILE_HEADER, (ROOT / 'include/ok/fs/persistent_sync_profile.hpp').read_text()),
            (export_sdk.NATIVE_MOUNT_INFO_HEADERS, export_sdk.NATIVE_MOUNT_INFO_PROFILE_HEADER, MOUNT_INFO_HEADER),
            (export_sdk.NATIVE_POWER_HEADERS, export_sdk.NATIVE_POWER_PROFILE_HEADER, POWER_HEADER),
            (export_sdk.NATIVE_METADATA_HEADERS, export_sdk.NATIVE_METADATA_PROFILE_HEADER, METADATA_HEADER),
            (export_sdk.NATIVE_SESSIONS_HEADERS, export_sdk.NATIVE_SESSIONS_PROFILE_HEADER, SESSIONS_HEADER),
            (export_sdk.NATIVE_GROUP_SIGNAL_HEADERS, export_sdk.NATIVE_GROUP_SIGNAL_PROFILE_HEADER, GROUP_SIGNAL_HEADER),
            (export_sdk.NATIVE_CONSOLE_CONTROL_HEADERS, export_sdk.NATIVE_CONSOLE_CONTROL_PROFILE_HEADER, CONSOLE_CONTROL_HEADER),
            (export_sdk.NATIVE_PTY_HEADERS, export_sdk.NATIVE_PTY_PROFILE_HEADER, PTY_HEADER)):
        for name in names:
            path = headers / name
            path.parent.mkdir(parents=True, exist_ok=True)
            if name == marker or not path.exists():
                path.write_text(declaration if name == marker else name)

MOUNT_INFO_HEADER = '#pragma once\n#include "ok/core/types.hpp"\n#include <string_view>\nnamespace ok::fs\n{\ninline constexpr std::string_view native_mount_snapshot_profile = "global-nine-slot-read-only-derived-generation-v1";\ninline constexpr u32 native_mount_snapshot_version = 1;\ninline constexpr u32 native_mount_snapshot_slots = 9;\ninline constexpr u32 native_mount_snapshot_path_bytes = 256;\ninline constexpr u32 native_mount_snapshot_wire_bytes = 288;\n} // namespace ok::fs\n'
MOUNT_INFO_UAPI = '#define OK_NATIVE_MOUNT_INFO_VERSION 1\n#define OK_MOUNT_INFO_CAPACITY 9U\n#define OK_MOUNT_INFO_PATH_CAPACITY 256U\n#define OK_MOUNT_STATE_UNUSED 0U\n#define OK_MOUNT_STATE_UNDERLYING_RAM 1U\n#define OK_MOUNT_STATE_BORROWED 2U\n#define OK_MOUNT_STATE_OWNED_PROVIDER 3U\n#define OK_MOUNT_STATE_OWNED_RAM 4U\n#define OK_MOUNT_STATE_DETACHED 5U\n#define OK_SYS_MOUNT_INFO 1034ULL\nstruct ok_mount_info\n{\n    uint32_t version;\n    uint32_t size;\n    uint32_t generation;\n    uint32_t slot;\n    uint32_t capacity;\n    uint32_t state;\n    uint32_t parent_id;\n    uint32_t path_length;\n    char path[OK_MOUNT_INFO_PATH_CAPACITY];\n};\n'
UAPI_HEADER = UAPI_HEADER.replace("#endif", MOUNT_INFO_UAPI + "#endif")

PTY_HEADER = '#pragma once\n#include "ok/core/types.hpp"\n#include <string_view>\nnamespace ok::user\n{\ninline constexpr std::string_view native_pty_profile = "bootcpu-native-raw-pair-ofd-waits-drain-eof-eio-v1";\ninline constexpr u32 native_pty_version = 1;\ninline constexpr u32 native_pty_module_abi = 75;\ninline constexpr u32 native_pty_pair_capacity = 8;\ninline constexpr u32 native_pty_queue_capacity = 4096;\ninline constexpr u32 native_pty_generation_bits = 64;\ninline constexpr u32 native_pty_descriptor_bytes = 4;\ninline constexpr u32 native_pty_pair_output_bytes = 8;\ninline constexpr u32 native_pty_serialized_boot_cpu = 1;\ninline constexpr u32 native_pty_raw = 1;\ninline constexpr u32 native_pty_retained_ofd_waits = 1;\ninline constexpr u32 native_pty_unconditional_hangup = 1;\ninline constexpr u32 native_pty_slave_master_closed_poll_error = 1;\ninline constexpr u32 native_pty_master_slave_closed_poll_error = 0;\ninline constexpr u32 native_pty_scm_rights = 0;\ninline constexpr u32 native_pty_linux_creation = 0;\ninline constexpr u32 native_pty_slave_reopen = 0;\ninline constexpr u32 native_pty_termios = 0;\ninline constexpr u32 native_pty_controlling_terminal = 0;\ninline constexpr u32 native_pty_terminal_signals = 0;\ninline constexpr u32 native_pty_job_control = 0;\ninline constexpr u32 native_pty_devpts = 0;\ninline constexpr u32 native_pty_smp = 0;\n} // namespace ok::user\n'
PTY_DEFINITIONS = (('OK_SYS_PTY_PAIR', '1035ULL'), ('OK_NATIVE_PTY_PAIR_VERSION', '1U'), ('OK_NATIVE_PTY_NONBLOCK', '0x0800U'), ('OK_NATIVE_PTY_CLOEXEC', '0x080000U'))
UAPI_HEADER = UAPI_HEADER.replace("#endif", "".join("#define " + n + " " + v + "\n" for n, v in PTY_DEFINITIONS) + "#endif")

def invalid_credentials_sources():
    """Use independent expected spellings, including preprocessing deception."""
    for name, value in CREDENTIALS_DEFINITIONS:
        line = '#define ' + name + ' ' + value + '\n'
        yield 'absent ' + name, UAPI_HEADER.replace(line, '')
        yield 'wrong ' + name, UAPI_HEADER.replace(line, '#define ' + name + ' 0\n')
        yield 'duplicate ' + name, UAPI_HEADER.replace('#endif', line + '#endif')
        yield 'function replacement ' + name, UAPI_HEADER.replace(
            line, '#define ' + name + '(value) ' + value + '\n')
        yield 'function duplicate ' + name, UAPI_HEADER.replace(
            '#endif', '#define ' + name + '() ' + value + '\n#endif')
        yield 'conditional ' + name, UAPI_HEADER.replace(line, '#if 0\n' + line + '#endif\n')
        yield 'quoted ' + name, UAPI_HEADER.replace(line, 'const char text[] = "' + line.rstrip() + '";\n')
        yield 'continued comment ' + name, UAPI_HEADER.replace(line, '// hidden definition\\\n' + line)
    for directive in ('#define UINT32_C(value) 0', '#define UINT32_C 0',
                      '#define UINT32_C(value) value', '#define OK_UAPI_SYSCALL_H(value) 0'):
        yield directive, UAPI_HEADER.replace('#endif', directive + '\n#endif')
    yield 'quoted contract', 'const char text[] = R"contract(\n' + UAPI_HEADER + ')contract";\n'
    yield 'comment contract', '/*\n' + UAPI_HEADER + '*/\n'
    yield 'conditional contract', '#if 0\n' + UAPI_HEADER + '#endif\n'
    yield 'digraph conditional', '%:if 0\n' + UAPI_HEADER + '%:endif\n'
    yield 'trigraph continuation', '// hidden contract??/\n' + UAPI_HEADER

CONSOLE_CONTROL_HEADER = '#pragma once\n#include "ok/core/types.hpp"\n#include <string_view>\nnamespace ok::user\n{\ninline constexpr std::string_view native_console_control_profile = "bootcpu-lease-epoch-explicit-session-foreground-eio-pid32-ioctl-v1";\ninline constexpr u32 native_console_control_version = 1;\ninline constexpr u32 native_console_control_module_abi = 75;\ninline constexpr u32 native_console_control_serialized_boot_cpu = 1;\ninline constexpr u32 native_console_control_lease_epoch = 1;\ninline constexpr u32 native_console_control_explicit_acquire = 1;\ninline constexpr u32 native_console_control_original_lease_session_leader = 1;\ninline constexpr u32 native_console_control_foreground_read = 1;\ninline constexpr u32 native_console_control_background_eio = 1;\ninline constexpr u32 native_console_control_same_session_background_control = 1;\ninline constexpr u32 native_console_control_zombie_group_until_reap = 1;\ninline constexpr u32 native_console_control_pid_bytes = 4;\ninline constexpr u32 native_console_control_native_uniform_requests = 1;\ninline constexpr u32 native_console_control_linux_generated_requests = 1;\ninline constexpr u32 native_console_control_auto_open_acquire = 0;\ninline constexpr u32 native_console_control_force_steal = 0;\ninline constexpr u32 native_console_control_empty_foreground_replaced = 0;\ninline constexpr u32 native_console_control_pty = 0;\ninline constexpr u32 native_console_control_termios = 0;\ninline constexpr u32 native_console_control_stop_continue = 0;\ninline constexpr u32 native_console_control_job_control = 0;\ninline constexpr u32 native_console_control_smp = 0;\n} // namespace ok::user\n'
CONSOLE_CONTROL_DEFINITIONS = (('OK_NATIVE_CONSOLE_CONTROL_VERSION', '1'), ('OK_SYS_IOCTL', '16ULL'), ('OK_TIOCSCTTY', '0x540eU'), ('OK_TIOCGSID', '0x5429U'), ('OK_TIOCGPGRP', '0x540fU'), ('OK_TIOCSPGRP', '0x5410U'), ('OK_EBADF', '9'), ('OK_EACCES', '13'), ('OK_EFAULT', '14'), ('OK_EIO', '5'), ('OK_EPERM', '1'), ('OK_ENOTTY', '25'), ('OK_EOPNOTSUPP', '95'))
UAPI_HEADER = UAPI_HEADER.replace("#endif", "".join("#define " + n + " " + v + "\n" for n,v in CONSOLE_CONTROL_DEFINITIONS) + "#endif")

def nondeclaration_marker_sources(declaration):
    """Exercise contexts that a comment-only regex mistakes for declarations."""
    sources = ["#if 0\n" + declaration + "#endif\n",
               "%:if 0\n" + declaration + "%:endif\n",
               "\v#if 0\n" + declaration + "#endif\n",
               "#define constexpr\n" + declaration,
               "#ifdef SDK_MARKERS\n" + declaration + "#endif\n",
               "#if 1\n// intentionally absent\n#else\n" + declaration + "#endif\n",
               "#i\\\nf 0\n" + declaration + "#endif\n",
               "// continued comment\\\n" + declaration.replace("\n", "\\\n") + "\n"]
    for prefix in ("R", "u8R", "uR", "UR", "LR"):
        sources.append('constexpr auto text = ' + prefix + '"markers(\n' + declaration + ')markers";\n')
    sources.append('constexpr auto text = R"tag(\n)ta\\\ng";\n' + declaration +
                   'constexpr auto another = R"tag(\n)tag";\n')
    for line in declaration.splitlines(keepends=True):
        field = line.split(" = ", 1)[0].rsplit(" ", 1)[1]
        sources.append("#define " + field + " unrelated_marker\n" + declaration)
        sources.append(declaration + "extern int " + field + " = 0;\n")
        sources.append(declaration + "extern int " + field + "{0};\n")
        sources.append(declaration + "extern int " + field + "(0);\n")
        sources.append(declaration + "extern int " + field + ";\n")
        sources.append("inline constexpr int " + field + "{0}\n" + declaration)
        sources.append(declaration + "inline constexpr int " + field + "{0}\n")
        sources.append("extern int " + field + "\n" + declaration)
        sources.append(declaration + "extern int " + field + "\n")
        sources.append(declaration + "inline constexpr int " + field + " [[maybe_unused]] = 0;\n")
        sources.append(declaration + "inline constexpr int " + field + "[1] = {0};\n")
        sources.append(declaration + "inline constexpr int (" + field + ") = 0;\n")
        sources.append(declaration + "inline constexpr int ((" + field + ")) = 0;\n")
        sources.append(declaration + "inline constexpr int " + field + " __attribute__((unused)) = 0;\n")
        sources.append(declaration + "[[maybe_unused]] inline constexpr int (" + field + ") = 0;\n")
        sources.append(declaration + "alignas(4) inline constexpr int (" + field + ") = 0;\n")
        sources.append(declaration + "inline constexpr decltype(0) (" + field + ") = 0;\n")
        sources.append(declaration + "[[maybe_unused]] int (" + field + ") = 0;\n")
    return sources

class NativeSignalProfile(unittest.TestCase):
    def test_abi74_profiles_refuse_immediate_stale72_and_future75(self):
        cases = ((export_sdk.COMMON_HEADERS, export_sdk.COMMON_HEADERS[0], COMMON_HEADER, export_sdk.validate_common_profile),
                 (export_sdk.UDP_PROFILE_HEADERS, export_sdk.UDP_PROFILE_HEADER, UDP_HEADER, export_sdk.validate_udp_profile),
                 (export_sdk.TCP_PROFILE_HEADERS, export_sdk.TCP_PROFILE_HEADER, TCP_HEADER, export_sdk.validate_tcp_profile),
                 (export_sdk.SHARED_FILE_PROFILE_HEADERS, export_sdk.SHARED_FILE_PROFILE_HEADER, SHARED_FILE_HEADER, export_sdk.validate_shared_file_profile),
                 (export_sdk.UNIX_PATHNAME_PROFILE_HEADERS, export_sdk.UNIX_PATHNAME_PROFILE_HEADER, UNIX_PATHNAME_HEADER, export_sdk.validate_unix_pathname_profile),
                 (export_sdk.DIRECTORY_CREATE_PROFILE_HEADERS, export_sdk.DIRECTORY_CREATE_PROFILE_HEADER, DIRECTORY_CREATE_HEADER, export_sdk.validate_directory_create_profile),
                 (export_sdk.FUTEX_WAKE_OPERATION_PROFILE_HEADERS, export_sdk.FUTEX_WAKE_OPERATION_PROFILE_HEADER, FUTEX_WAKE_OPERATION_HEADER, export_sdk.validate_futex_wake_operation_profile),
                 (export_sdk.FUTEX_REQUEUE_PROFILE_HEADERS, export_sdk.FUTEX_REQUEUE_PROFILE_HEADER, FUTEX_REQUEUE_HEADER, export_sdk.validate_futex_requeue_profile))
        for names, marker_name, declaration, validate in cases:
            self.assertIn("module_abi = 75;", declaration)
            with tempfile.TemporaryDirectory() as directory:
                headers = Path(directory)
                for name in names:
                    path = headers / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(declaration if name == marker_name else name)
                marker = headers / marker_name
                validate(headers)
                for version in (69, 70, 71, 72, 73, 74, 76):
                    marker.write_text(declaration.replace("module_abi = 75;", f"module_abi = {version};"))
                    with self.subTest(marker=marker_name, version=version), self.assertRaises(ValueError):
                        validate(headers)
                marker.write_text(declaration)
                validate(headers)

    def test_native_credentials_requires_exact_independent_contract(self):
        self.assertEqual(export_sdk.NATIVE_CREDENTIALS_PROFILE, CREDENTIALS_PROFILE)
        self.assertEqual(export_sdk.NATIVE_CREDENTIALS_HEADERS, CREDENTIALS_HEADERS)
        self.assertEqual(export_sdk.NATIVE_CREDENTIALS_DEFINES, dict(CREDENTIALS_DEFINITIONS))
        with tempfile.TemporaryDirectory() as directory:
            headers = Path(directory)
            write_pathname_and_directory_headers(headers)
            export_sdk.validate_native_credentials_profile(headers)
            for name in CREDENTIALS_HEADERS:
                path = headers / name
                original = path.read_bytes()
                path.unlink()
                with self.subTest(missing=name), self.assertRaisesRegex(ValueError, 'native credentials header missing'):
                    export_sdk.validate_native_credentials_profile(headers)
                path.write_bytes(original)

    def test_native_credentials_refuses_inactive_and_deceptive_UAPI(self):
        with tempfile.TemporaryDirectory() as directory:
            headers = Path(directory)
            write_pathname_and_directory_headers(headers)
            path = headers / 'ok/uapi/syscall.h'
            for label, source in invalid_credentials_sources():
                path.write_text(source)
                with self.subTest(label=label), self.assertRaisesRegex(ValueError, '(native credentials UAPI|UAPI version)'):
                    export_sdk.validate_native_credentials_profile(headers)

    def test_native_termination_requires_unconditional_exact_UAPI(self):
        with tempfile.TemporaryDirectory() as directory:
            headers = Path(directory)
            path = headers / 'ok/uapi/signal.h'
            path.parent.mkdir(parents=True)
            path.write_text(TERMINATION_HEADER)
            export_sdk.validate_native_termination_profile(headers)
            for source in ('#if 0\n' + TERMINATION_HEADER + '#endif\n',
                           TERMINATION_HEADER.replace('OK_SIGTERM 15', 'OK_SIGTERM 10'),
                           TERMINATION_HEADER.replace('OK_NATIVE_TERMINATION_VERSION 1', 'OK_NATIVE_TERMINATION_VERSION 2'),
                           TERMINATION_HEADER + '#define OK_SIGKILL 9\n',
                           '/*\n' + TERMINATION_HEADER + '*/\n'):
                path.write_text(source)
                with self.subTest(source=source), self.assertRaisesRegex(ValueError, 'native termination'):
                    export_sdk.validate_native_termination_profile(headers)

    def test_all_native_markers_reject_conditional_raw_string_and_malformed_duplicate_declarations(self):
        cases = ((export_sdk.COMMON_HEADERS, export_sdk.COMMON_HEADERS[0], COMMON_HEADER, export_sdk.validate_common_profile),
                 (export_sdk.UDP_PROFILE_HEADERS, export_sdk.UDP_PROFILE_HEADER, UDP_HEADER, export_sdk.validate_udp_profile),
                 (export_sdk.TCP_PROFILE_HEADERS, export_sdk.TCP_PROFILE_HEADER, TCP_HEADER, export_sdk.validate_tcp_profile),
                 (export_sdk.SHARED_FILE_PROFILE_HEADERS, export_sdk.SHARED_FILE_PROFILE_HEADER, SHARED_FILE_HEADER, export_sdk.validate_shared_file_profile),
                 (export_sdk.UNIX_PATHNAME_PROFILE_HEADERS, export_sdk.UNIX_PATHNAME_PROFILE_HEADER, UNIX_PATHNAME_HEADER, export_sdk.validate_unix_pathname_profile),
                 (export_sdk.DIRECTORY_CREATE_PROFILE_HEADERS, export_sdk.DIRECTORY_CREATE_PROFILE_HEADER, DIRECTORY_CREATE_HEADER, export_sdk.validate_directory_create_profile),
                 (export_sdk.FUTEX_WAKE_OPERATION_PROFILE_HEADERS, export_sdk.FUTEX_WAKE_OPERATION_PROFILE_HEADER, FUTEX_WAKE_OPERATION_HEADER, export_sdk.validate_futex_wake_operation_profile),
                 (export_sdk.FUTEX_REQUEUE_PROFILE_HEADERS, export_sdk.FUTEX_REQUEUE_PROFILE_HEADER, FUTEX_REQUEUE_HEADER, export_sdk.validate_futex_requeue_profile))
        for names, marker_name, declaration, validate in cases:
            with tempfile.TemporaryDirectory() as directory:
                headers = Path(directory)
                for name in names:
                    path = headers / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(declaration if name == marker_name else name)
                marker = headers / marker_name
                validate(headers)
                for source in nondeclaration_marker_sources(declaration):
                    marker.write_text(source)
                    with self.subTest(marker=marker_name, source=source), self.assertRaises(ValueError):
                        validate(headers)
                marker.write_text('constexpr auto example = R"example(\n' + declaration + ')example";\n' + declaration)
                validate(headers)
        with tempfile.TemporaryDirectory() as directory:
            header = Path(directory) / "module.hpp"
            declaration = "inline constexpr u32 kernel_module_abi_version = 75;\n"
            for source in nondeclaration_marker_sources(declaration):
                header.write_text(source)
                with self.subTest(marker="module.hpp", source=source), self.assertRaises(ValueError):
                    export_sdk.validate_module_abi(header)
            header.write_text(declaration)
            export_sdk.validate_module_abi(header)

    def test_existing_native_contracts_require_real_typed_markers_and_ABI75(self):
        cases = ((export_sdk.COMMON_HEADERS, export_sdk.COMMON_HEADERS[0], COMMON_HEADER, export_sdk.validate_common_profile),
                 (export_sdk.UDP_PROFILE_HEADERS, export_sdk.UDP_PROFILE_HEADER, UDP_HEADER, export_sdk.validate_udp_profile),
                 (export_sdk.SHARED_FILE_PROFILE_HEADERS, export_sdk.SHARED_FILE_PROFILE_HEADER, SHARED_FILE_HEADER, export_sdk.validate_shared_file_profile),
                 (export_sdk.UNIX_PATHNAME_PROFILE_HEADERS, export_sdk.UNIX_PATHNAME_PROFILE_HEADER, UNIX_PATHNAME_HEADER, export_sdk.validate_unix_pathname_profile),
                 (export_sdk.DIRECTORY_CREATE_PROFILE_HEADERS, export_sdk.DIRECTORY_CREATE_PROFILE_HEADER, DIRECTORY_CREATE_HEADER, export_sdk.validate_directory_create_profile),
                 (export_sdk.FUTEX_WAKE_OPERATION_PROFILE_HEADERS, export_sdk.FUTEX_WAKE_OPERATION_PROFILE_HEADER, FUTEX_WAKE_OPERATION_HEADER, export_sdk.validate_futex_wake_operation_profile),
                 (export_sdk.FUTEX_REQUEUE_PROFILE_HEADERS, export_sdk.FUTEX_REQUEUE_PROFILE_HEADER, FUTEX_REQUEUE_HEADER, export_sdk.validate_futex_requeue_profile))
        for names, marker_name, declaration, validate in cases:
            with tempfile.TemporaryDirectory() as directory:
                headers = Path(directory)
                for name in names:
                    path = headers / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(declaration if name == marker_name else name)
                validate(headers)
                marker = headers / marker_name
                bad = ["// " + declaration.replace("\n", "\n// "), "/*\n" + declaration + "*/\n",
                       declaration.replace("inline constexpr", "extern"),
                       declaration.replace("module_abi = 75", "module_abi = 59"),
                       declaration.replace("module_abi = 75", "module_abi = 61"),
                       declaration.replace("module_abi = 75", "module_abi = 62"),
                       declaration.replace("module_abi = 75", "module_abi = 64"),
                       declaration.replace("module_abi = 75", "module_abi = 65"),
                       declaration.replace("module_abi = 75", "module_abi = 76")]
                for line in declaration.splitlines(keepends=True):
                    field = line.split(" = ", 1)[0].rsplit(" ", 1)[1]
                    bad.append(declaration.replace(field, "unrelated_" + field))
                    bad.append(declaration.replace(line, "// " + line))
                for source in bad:
                    marker.write_text(source)
                    with self.subTest(marker=marker_name, source=source), self.assertRaises(ValueError):
                        validate(headers)

    def test_tcp_accept_real_typed_profiles_and_all_capacities_are_required(self):
        with tempfile.TemporaryDirectory() as directory:
            headers = Path(directory)
            for name in export_sdk.TCP_PROFILE_HEADERS:
                path = headers / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(TCP_HEADER if name == export_sdk.TCP_PROFILE_HEADER else name)
            marker = headers / export_sdk.TCP_PROFILE_HEADER
            bad = ["// " + TCP_HEADER.replace("\n", "\n// "), "/*\n" + TCP_HEADER + "*/\n",
                TCP_HEADER.replace(" native_tcp_", " unrelated_native_tcp_").replace(" linux_tcp_", " unrelated_linux_tcp_"),
                TCP_HEADER.replace("inline constexpr", "extern"),
                TCP_HEADER.replace("module_abi = 75", "module_abi = 59"),
                TCP_HEADER.replace("module_abi = 75", "module_abi = 61"),
                TCP_HEADER.replace("module_abi = 75", "module_abi = 62"),
                TCP_HEADER.replace("module_abi = 75", "module_abi = 64"),
                TCP_HEADER.replace("module_abi = 75", "module_abi = 65"),
                TCP_HEADER.replace("module_abi = 75", "module_abi = 76")]
            for line in TCP_HEADER.splitlines(keepends=True):
                if "capacity" in line or "poll_budget" in line:
                    bad.append(TCP_HEADER.replace(line, line.replace(" = ", " = 9")))
                if "accept_profile" in line:
                    bad.append(TCP_HEADER.replace(line, "// " + line))
                    bad.append(TCP_HEADER.replace(line, line.replace("reserved-accept-v1", "blocking-full-v1")))
            for source in bad:
                marker.write_text(source)
                with self.subTest(source=source), self.assertRaisesRegex(ValueError, "TCP"):
                    export_sdk.validate_tcp_profile(headers)

    def test_tcp_profile_requires_complete_bounded_contract(self):
        with tempfile.TemporaryDirectory() as directory:
            headers = Path(directory)
            for name in export_sdk.TCP_PROFILE_HEADERS:
                path = headers / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(TCP_HEADER if name == export_sdk.TCP_PROFILE_HEADER else name)
            export_sdk.validate_tcp_profile(headers)
            marker = headers / export_sdk.TCP_PROFILE_HEADER
            for declaration in TCP_HEADER.splitlines(keepends=True):
                for source in (TCP_HEADER.replace(declaration, ""), TCP_HEADER + declaration):
                    marker.write_text(source)
                    with self.subTest(declaration=declaration, source=source), self.assertRaisesRegex(ValueError, "TCP"):
                        export_sdk.validate_tcp_profile(headers)
            for old, wrong in (("module_abi = 75", "module_abi = 55"),
                               ("module_abi = 75", "module_abi = 56"),
                               ("module_abi = 75", "module_abi = 59"),
                               ("nonblocking-immediate-v1", "blocking-full-v1"),
                               ("segment_capacity = 536", "segment_capacity = 1460"),
                               ("poll_budget = 8", "poll_budget = 16")):
                marker.write_text(TCP_HEADER.replace(old, wrong))
                with self.subTest(wrong=wrong), self.assertRaisesRegex(ValueError, "TCP"):
                    export_sdk.validate_tcp_profile(headers)
            marker.write_text(TCP_HEADER)
            for name in export_sdk.TCP_PROFILE_HEADERS:
                path = headers / name
                content = path.read_text()
                path.unlink()
                with self.subTest(missing=name), self.assertRaisesRegex(ValueError, "TCP contract header"):
                    export_sdk.validate_tcp_profile(headers)
                path.write_text(content)

    def test_tcp_refusal_precedes_export_staging(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            headers = root / "headers"
            declarations = {
                "ok/core/module.hpp": "inline constexpr u32 kernel_module_abi_version = 75;\n",
                "ok/sched/scheduler.hpp": "user_init_pid = 1; create_init_process(); user_adoption_target();",
                "ok/uapi/syscall.h": UAPI_HEADER,
                export_sdk.COMMON_HEADERS[0]: COMMON_HEADER,
                export_sdk.UDP_PROFILE_HEADER: UDP_HEADER,
                export_sdk.TCP_PROFILE_HEADER: TCP_HEADER,
                export_sdk.SHARED_FILE_PROFILE_HEADER: SHARED_FILE_HEADER,
            }
            for name in set(export_sdk.COMMON_HEADERS + export_sdk.UDP_PROFILE_HEADERS + export_sdk.TCP_PROFILE_HEADERS + export_sdk.SHARED_FILE_PROFILE_HEADERS) | set(declarations):
                path = headers / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(declarations.get(name, name))
            write_pathname_and_directory_headers(headers)
            output = root / "sdk"
            output.mkdir()
            previous = output / "manifest.json"
            previous.write_bytes(b"preserve previous export")
            args = argparse.Namespace(output=output, mode="release", define=[RUNTIME], asflag=[],
                                      cxxflag=[], arch="x86_64", headers=headers)
            for invalid in ("", TCP_HEADER.replace("module_abi = 75", "module_abi = 55"), TCP_HEADER + TCP_HEADER):
                (headers / export_sdk.TCP_PROFILE_HEADER).write_text(invalid)
                with self.subTest(source=invalid), self.assertRaisesRegex(ValueError, "TCP"):
                    export_sdk.export(args)
                self.assertEqual(previous.read_bytes(), b"preserve previous export")
                self.assertEqual(sorted(path.name for path in root.iterdir()), ["headers", "sdk"])

    def test_previous_zero_port_gap_profiles_are_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            headers = Path(directory)
            for name in export_sdk.UDP_PROFILE_HEADERS:
                path = headers / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(UDP_HEADER if name == export_sdk.UDP_PROFILE_HEADER else name)
            marker = headers / export_sdk.UDP_PROFILE_HEADER
            for old, current in (('bootcpu-static-peer-ipv4-polling-captured-connected-v2', 'bootcpu-static-peer-ipv4-polling-captured-connected-zero-send-v3'), ('bootcpu-static-peer-ipv4-captured-connected-v2', 'bootcpu-static-peer-ipv4-captured-connected-zero-send-v3'), ('bounded-static-peer-association-port-zero-wire-gap-v1', 'bounded-static-peer-association-zero-destination-output-v2')):
                self.assertEqual(UDP_HEADER.count(current), 1)
                marker.write_text(UDP_HEADER.replace(current, old))
                with self.subTest(old=old), self.assertRaisesRegex(ValueError, "UDP profile mismatch"):
                    export_sdk.validate_udp_profile(headers)

    def test_connected_capability_header_refusals(self):
        declaration = 'inline constexpr std::string_view native_udp_connected_profile = "bounded-static-peer-association-zero-destination-output-v2";\n'
        invalid = (UDP_HEADER.replace(declaration, ""),
                   UDP_HEADER.replace(declaration, declaration + declaration),
                   UDP_HEADER.replace('bounded-static-peer-association-zero-destination-output-v2', "bounded-static-peer-association-v0"))
        with tempfile.TemporaryDirectory() as directory:
            headers = Path(directory)
            for name in export_sdk.UDP_PROFILE_HEADERS:
                path = headers / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(UDP_HEADER if name == export_sdk.UDP_PROFILE_HEADER else name)
            marker = headers / export_sdk.UDP_PROFILE_HEADER
            for content in invalid:
                marker.write_text(content)
                with self.subTest(content=content), self.assertRaisesRegex(ValueError, "UDP profile mismatch"):
                    export_sdk.validate_udp_profile(headers)

    def test_udp_headers_and_exact_profile_markers(self):
        with tempfile.TemporaryDirectory() as directory:
            headers = Path(directory)
            for name in export_sdk.UDP_PROFILE_HEADERS:
                path = headers / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(UDP_HEADER if name == export_sdk.UDP_PROFILE_HEADER else name)
            export_sdk.validate_udp_profile(headers)
            marker = headers / export_sdk.UDP_PROFILE_HEADER
            for source in (UDP_HEADER.replace("module_abi = 75", "module_abi = 49"),
                           UDP_HEADER.replace("module_abi = 75", "module_abi = 51"),
                           UDP_HEADER.replace("module_abi = 75", "module_abi = 52"),
                        UDP_HEADER.replace("module_abi = 75", "module_abi = 53"),
                        UDP_HEADER.replace("module_abi = 75", "module_abi = 54"),
                        UDP_HEADER.replace("module_abi = 75", "module_abi = 55"),
                        UDP_HEADER.replace("module_abi = 75", "module_abi = 56"),
                           UDP_HEADER.replace("capacity = 16", "capacity = 15"),
                           UDP_HEADER.replace("capacity = 2", "capacity = 1"),
                           UDP_HEADER.replace("capacity = 1472", "capacity = 1500"),
                           UDP_HEADER.replace("capacity = 1514", "capacity = 1518"),
                           UDP_HEADER.replace("bootcpu-static-peer-ipv4-captured-connected-zero-send-v3", "bootcpu-static-peer-ipv4-nonblocking-immediate-v1"),
                           UDP_HEADER.replace("bootcpu-static-peer-ipv4-polling-captured-connected-zero-send-v3", "bootcpu-static-peer-ipv4-polling-captured-receive-v1"),
                           UDP_HEADER + UDP_HEADER):
                marker.write_text(source)
                with self.subTest(source=source), self.assertRaisesRegex(ValueError, "UDP"):
                    export_sdk.validate_udp_profile(headers)
            marker.write_text(UDP_HEADER)
            for name in export_sdk.UDP_PROFILE_HEADERS:
                path = headers / name
                content = path.read_text()
                path.unlink()
                with self.subTest(missing=name), self.assertRaisesRegex(ValueError, "UDP contract header"):
                    export_sdk.validate_udp_profile(headers)
                path.write_text(content)

    def test_udp_refusal_precedes_atomic_export_staging(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            headers = root / "headers"
            for name, content in (("ok/core/module.hpp", "inline constexpr u32 kernel_module_abi_version = 75;"),
                                  ("ok/sched/scheduler.hpp", "user_init_pid = 1;\ncreate_init_process();\nuser_adoption_target();\n"),
                                  ("ok/uapi/syscall.h", UAPI_HEADER)):
                path = headers / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            for name in (*export_sdk.COMMON_HEADERS, *export_sdk.UDP_PROFILE_HEADERS):
                path = headers / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(COMMON_HEADER if name.endswith("ext4_mounted_common_profile.hpp") else
                                UDP_HEADER if name == export_sdk.UDP_PROFILE_HEADER else name)
            write_pathname_and_directory_headers(headers)
            output = root / "sdk"
            output.mkdir()
            old = output / "manifest.json"
            old.write_text('{"existing":true}\n')
            args = argparse.Namespace(output=output, mode="release", define=[RUNTIME], asflag=[], cxxflag=[],
                                      arch="x86_64", headers=headers)
            marker = headers / export_sdk.UDP_PROFILE_HEADER
            for source in ("", UDP_HEADER.replace("module_abi = 75", "module_abi = 52"),
                        UDP_HEADER.replace("module_abi = 75", "module_abi = 53"),
                        UDP_HEADER.replace("module_abi = 75", "module_abi = 54"),
                        UDP_HEADER.replace("module_abi = 75", "module_abi = 55"),
                        UDP_HEADER.replace("module_abi = 75", "module_abi = 56"), UDP_HEADER + UDP_HEADER):
                marker.write_text(source)
                with self.subTest(source=source), self.assertRaisesRegex(ValueError, "UDP"):
                    export_sdk.export(args)
                self.assertEqual(old.read_text(), '{"existing":true}\n')
                self.assertEqual(sorted(path.name for path in root.iterdir()), ["headers", "sdk"])

    def test_common_profile_requires_original_headers_and_exact_layout_markers(self):
        with tempfile.TemporaryDirectory() as directory:
            headers = Path(directory)
            for name in export_sdk.COMMON_HEADERS:
                path = headers / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(COMMON_HEADER if name.endswith('ext4_mounted_common_profile.hpp') else name)
            export_sdk.validate_common_profile(headers)
            marker = headers / 'ok/fs/ext4_mounted_common_profile.hpp'
            for source in (COMMON_HEADER.replace('module_abi = 75', 'module_abi = 51'),
                           COMMON_HEADER.replace('module_abi = 75', 'module_abi = 52'),
                        COMMON_HEADER.replace('module_abi = 75', 'module_abi = 53'),
                        COMMON_HEADER.replace('module_abi = 75', 'module_abi = 54'),
                        COMMON_HEADER.replace('module_abi = 75', 'module_abi = 55'),
                        COMMON_HEADER.replace("module_abi = 75", "module_abi = 56"),
                           COMMON_HEADER.replace('counter_bits = 64', 'counter_bits = 32'),
                           COMMON_HEADER.replace('optional-revoke-legacy-v4', 'unrestricted-v1'),
                           COMMON_HEADER + COMMON_HEADER, ''):
                marker.write_text(source)
                with self.subTest(source=source), self.assertRaisesRegex(ValueError, 'common-opening'):
                    export_sdk.validate_common_profile(headers)
            marker.write_text(COMMON_HEADER)
            for name in export_sdk.COMMON_HEADERS:
                path = headers / name
                saved = path.read_bytes()
                path.unlink()
                with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'header is missing'):
                    export_sdk.validate_common_profile(headers)
                path.write_bytes(saved)

    def test_bios_extent_requires_versioned_loader_and_exact_image_size(self):
        capacity = 512 * 32 * 512
        boot = bytearray(512)
        boot[0x1f0:0x1f8] = b'OKB2' + capacity.to_bytes(4, 'little')
        boot[-2:] = b'\x55\xaa'
        self.assertEqual(export_sdk.validate_bios_extent(boot, 512 + capacity), capacity)
        for size in (512 + capacity - 1, 512 + capacity + 512):
            with self.subTest(size=size), self.assertRaisesRegex(ValueError, 'BIOS image'):
                export_sdk.validate_bios_extent(boot, size)
        for offset, value in ((0x1f0, ord('X')), (0x1f6, 0), (510, 0)):
            corrupted = bytearray(boot)
            corrupted[offset] = value
            with self.subTest(offset=offset), self.assertRaisesRegex(ValueError, 'BIOS boot sector'):
                export_sdk.validate_bios_extent(corrupted, 512 + capacity)

    def test_all_architecture_modes(self):
        for arch in ARCHES:
            for mode in ('debug', 'release'):
                with self.subTest(arch=arch, mode=mode):
                    defines = [RUNTIME] + ([TESTS] if mode == 'debug' else [])
                    asflags = ['-D' + RUNTIME] if arch == 'mips64' else []
                    self.assertEqual(export_sdk.native_signal_profile(mode, defines, asflags, arch), 'bounded-bootcpu-v1')
                    self.assertEqual(export_sdk.native_signal_profile(mode, [], [], arch), 'disabled')

    def test_private_debug_and_canonical_definitions(self):
        for arch in ARCHES:
            asflags = ['-D' + RUNTIME] if arch == 'mips64' else []
            self.assertEqual(export_sdk.native_signal_profile('debug', [RUNTIME, PRIVATE, TESTS], asflags, arch), 'bounded-bootcpu-v1')
        for defines in ([RUNTIME + '=1'], [RUNTIME, RUNTIME], [RUNTIME, RUNTIME + '=1']):
            with self.subTest(defines=defines), self.assertRaises(ValueError):
                export_sdk.native_signal_profile('release', defines, [], 'x86_64')

    def test_compiler_macro_overrides(self):
        for flags in (['-D' + RUNTIME, '-U' + RUNTIME], ['-U', RUNTIME], ['-U ' + RUNTIME],
                      ['-D' + RUNTIME + '=1'], ['-D' + RUNTIME, '-D' + RUNTIME]):
            with self.subTest(flags=flags), self.assertRaises(ValueError):
                export_sdk.native_signal_profile('release', [RUNTIME], flags, 'mips64')
        for flags in (['-D' + RUNTIME], ['-D' + TESTS], ['-D', TESTS], ['-D ' + PRIVATE], ['-U' + RUNTIME]):
            with self.subTest(cxxflags=flags), self.assertRaises(ValueError):
                export_sdk.native_signal_profile('release', [RUNTIME], [], 'x86_64', flags)

    def test_preprocessor_forwarded_macro_overrides(self):
        for flag in ('-Wp,-U' + RUNTIME, '-Wp,-U,' + RUNTIME,
                     '-Wp,-D' + TESTS, '-Wp,-D,' + TESTS):
            for target in ('asflags', 'cxxflags'):
                with self.subTest(flag=flag, target=target), self.assertRaises(ValueError):
                    export_sdk.native_signal_profile('release', [RUNTIME],
                        [flag] if target == 'asflags' else [], 'x86_64', [flag] if target == 'cxxflags' else [])

    def test_emitted_manifest_capability_and_artifact_hashes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            headers = root / 'headers'
            headers.mkdir()
            (headers / 'ok/core').mkdir(parents=True)
            (headers / 'ok/sched').mkdir(parents=True)
            (headers / 'ok/uapi').mkdir(parents=True)
            (headers / 'ok/core/module.hpp').write_text('inline constexpr u32 kernel_module_abi_version = 75;\n')
            (headers / 'ok/sched/scheduler.hpp').write_text(
                'user_init_pid = 1;\ncreate_init_process();\nuser_adoption_target();\n')
            (headers / 'ok/uapi/syscall.h').write_text(UAPI_HEADER)
            (headers / 'ok/uapi/signal.h').write_text(TERMINATION_HEADER)
            for name in export_sdk.COMMON_HEADERS:
                header = headers / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text(COMMON_HEADER if name.endswith('ext4_mounted_common_profile.hpp') else name)
            for name in export_sdk.UDP_PROFILE_HEADERS:
                header = headers / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text(UDP_HEADER if name == export_sdk.UDP_PROFILE_HEADER else name)
            for name in export_sdk.TCP_PROFILE_HEADERS:
                header = headers / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text(TCP_HEADER if name == export_sdk.TCP_PROFILE_HEADER else name)
            for name in export_sdk.SHARED_FILE_PROFILE_HEADERS:
                header = headers / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text(SHARED_FILE_HEADER if name == export_sdk.SHARED_FILE_PROFILE_HEADER else name)
            write_pathname_and_directory_headers(headers)
            payload = root / 'payload'
            payload.write_bytes(b'non-executed packaging fixture')
            args = argparse.Namespace(output=root / 'sdk', mode='release', define=[RUNTIME],
                asflag=[], cxxflag=[], ldflag=[], arch='rv64', headers=headers, library=payload,
                ar=Path(shutil.which('ar')), boot_object=[payload], linker_script=payload,
                kernel_bin=payload, kernel_elf=payload, image_format='elf', compiler=Path('/mock/compiler'),
                triple='riscv64-elf', version='fixture-only')
            with patch.object(export_sdk.subprocess, 'check_output', side_effect=['riscv64-elf\n', '14.2.0\n']):
                export_sdk.export(args)
            manifest = json.loads((args.output / 'manifest.json').read_text())
            self.assertEqual(manifest['native_tcp_profile'], export_sdk.NATIVE_TCP_PROFILE)
            self.assertEqual(manifest['linux_tcp_profile'], export_sdk.LINUX_TCP_PROFILE)
            self.assertEqual(manifest['native_tcp_accept_profile'], export_sdk.NATIVE_TCP_ACCEPT_PROFILE)
            self.assertEqual(manifest['linux_tcp_accept_profile'], export_sdk.LINUX_TCP_ACCEPT_PROFILE)
            for name in export_sdk.TCP_PROFILE_HEADERS:
                self.assertIn('include/' + name, manifest['files'])
            self.assertEqual(manifest['native_unix_pathname_profile'], export_sdk.UNIX_PATHNAME_PROFILE)
            self.assertEqual(manifest['linux_unix_pathname_profile'], export_sdk.UNIX_PATHNAME_PROFILE)
            self.assertEqual(manifest['ext4_directory_create_profile'], export_sdk.DIRECTORY_CREATE_PROFILE)
            for name in (*export_sdk.UNIX_PATHNAME_PROFILE_HEADERS, *export_sdk.DIRECTORY_CREATE_PROFILE_HEADERS):
                self.assertIn('include/' + name, manifest['files'])
            self.assertEqual(manifest['native_signal_profile'], 'bounded-bootcpu-v1')
            self.assertEqual(manifest['native_init_profile'], 'designated-pid1-native-linux-adoption-v2')
            self.assertEqual(manifest['native_termination_profile'], export_sdk.NATIVE_TERMINATION_PROFILE)
            self.assertEqual(manifest['native_credentials_profile'], CREDENTIALS_PROFILE)
            self.assertEqual(manifest['native_power_profile'], 'original-init-scalar-prepare-queued-commit-v1')
            self.assertIn('include/ok/user/native_power_profile.hpp', manifest['files'])
            for name in CREDENTIALS_HEADERS:
                self.assertIn('include/' + name, manifest['files'])
            self.assertIn('include/ok/uapi/signal.h', manifest['files'])
            self.assertEqual(manifest['defines'], [RUNTIME])
            self.assertEqual(manifest['abi_version'], 75)
            self.assertEqual(manifest['native_pty_profile'], export_sdk.NATIVE_PTY_PROFILE)
            self.assertEqual(manifest['native_sessions_profile'], export_sdk.NATIVE_SESSIONS_PROFILE)
            self.assertEqual(manifest['native_group_signal_profile'], export_sdk.NATIVE_GROUP_SIGNAL_PROFILE)
            self.assertEqual(manifest['native_console_control_profile'], export_sdk.NATIVE_CONSOLE_CONTROL_PROFILE)
            for name in export_sdk.NATIVE_CONSOLE_CONTROL_HEADERS:
                self.assertIn('include/' + name, manifest['files'])
            for name in export_sdk.NATIVE_GROUP_SIGNAL_HEADERS:
                self.assertIn('include/' + name, manifest['files'])
            for name in export_sdk.NATIVE_SESSIONS_HEADERS:
                self.assertIn('include/' + name, manifest['files'])
            self.assertEqual(manifest['native_futex_wake_operation_profile'], export_sdk.FUTEX_WAKE_OPERATION_PROFILE)
            self.assertEqual(manifest['linux_futex_wake_operation_profile'], export_sdk.FUTEX_WAKE_OPERATION_PROFILE)
            self.assertEqual(manifest['native_futex_requeue_profile'], export_sdk.FUTEX_REQUEUE_PROFILE)
            self.assertEqual(manifest['linux_futex_requeue_profile'], export_sdk.FUTEX_REQUEUE_PROFILE)
            for name in export_sdk.FUTEX_REQUEUE_PROFILE_HEADERS:
                self.assertIn('include/' + name, manifest['files'])
            for name in export_sdk.FUTEX_WAKE_OPERATION_PROFILE_HEADERS:
                self.assertIn('include/' + name, manifest['files'])
            self.assertEqual(manifest['ext4_common_opening_profile'],
                             'serialized-common-eight-family-directory-create-ordered-free-init-zero-refill-optional-revoke-legacy-v4')
            for name in export_sdk.COMMON_HEADERS:
                self.assertIn('include/' + name, manifest['files'])
            self.assertEqual(manifest["native_udp_profile"], export_sdk.NATIVE_UDP_PROFILE)
            self.assertEqual(manifest["linux_udp_profile"], export_sdk.LINUX_UDP_PROFILE)
            self.assertEqual(manifest["native_udp_connected_profile"], export_sdk.CONNECTED_UDP_PROFILE)
            for name in export_sdk.UDP_PROFILE_HEADERS:
                self.assertIn("include/" + name, manifest["files"])
            self.assertEqual(manifest['ext4_directory_rename_profile'],
                             'bounded-mounted-dirty-same-dir-absent-fit-or-relocate-ordered-home-v2')
            self.assertEqual(manifest['ext4_empty_create_profile'],
                             'bounded-mounted-dirty-empty-regular-initial-attrs-ordered-home-v1')
            self.assertEqual(manifest['ext4_first_data_profile'],
                             'bounded-mounted-dirty-fresh-empty-first-block-ordered-home-v1')
            self.assertEqual(manifest['ext4_next_data_profile'],
                             'bounded-mounted-dirty-aligned-inline-next-block-ordered-home-v1')
            self.assertIn('lib/libokernel_boot.a', manifest['files'])
            for name, digest in manifest['files'].items():
                self.assertEqual(hashlib.sha256((args.output / name).read_bytes()).hexdigest(), digest)
            previous = {str(path.relative_to(args.output)): path.read_bytes()
                        for path in args.output.rglob('*') if path.is_file()}
            # Capability rejection must happen before staging or replacing the
            # existing export, independent of compiler/process success mocks.
            for label, source in invalid_credentials_sources():
                (headers / 'ok/uapi/syscall.h').write_text(source)
                with self.subTest(refused_before_publication=label), self.assertRaisesRegex(
                        ValueError, '(native credentials UAPI|UAPI version)'):
                    export_sdk.export(args)
                self.assertEqual({str(path.relative_to(args.output)): path.read_bytes()
                                  for path in args.output.rglob('*') if path.is_file()}, previous)
                self.assertFalse(any(path.name.startswith('.kernel-sdk-') for path in root.iterdir()))
            for name, value in POWER_DEFINITIONS:
                line = '#define ' + name + ' ' + value + '\n'
                for source in (UAPI_HEADER.replace(line, ''),
                               UAPI_HEADER.replace(line, '#define ' + name + ' 0\n'),
                               UAPI_HEADER.replace('#endif', line + '#endif')):
                    (headers / 'ok/uapi/syscall.h').write_text(source)
                    with self.subTest(power_refused_before_publication=name), self.assertRaisesRegex(ValueError, 'native power UAPI'):
                        export_sdk.export(args)
                    self.assertEqual({str(path.relative_to(args.output)): path.read_bytes()
                                      for path in args.output.rglob('*') if path.is_file()}, previous)
                    self.assertFalse(any(path.name.startswith('.kernel-sdk-') for path in root.iterdir()))

    def test_uapi_version_requires_exact_revision(self):
        with tempfile.TemporaryDirectory() as directory:
            header = Path(directory) / 'syscall.h'
            header.write_text(UAPI_HEADER)
            export_sdk.validate_uapi_version(header)
            for source in (UAPI_HEADER.replace('MINOR 16', 'MINOR 9'),
                           UAPI_HEADER.replace('MINOR 16', 'MINOR 17'),
                           UAPI_HEADER.replace('PATCH 0', 'PATCH 1'),
                           UAPI_HEADER.replace('MAJOR 0', 'MAJOR 1'),
                           UAPI_HEADER.replace('#define OK_UAPI_VERSION_MINOR 16\n', ''),
                           UAPI_HEADER + '#define OK_UAPI_VERSION_MINOR 16\n'):
                header.write_text(source)
                with self.subTest(source=source), self.assertRaisesRegex(ValueError, 'UAPI 0.16.0'):
                    export_sdk.validate_uapi_version(header)

    def test_abi_and_uapi_mismatch_cannot_replace_export(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            headers = root / 'headers'
            (headers / 'ok/core').mkdir(parents=True)
            (headers / 'ok/sched').mkdir(parents=True)
            (headers / 'ok/uapi').mkdir(parents=True)
            module = headers / 'ok/core/module.hpp'
            scheduler = headers / 'ok/sched/scheduler.hpp'
            uapi = headers / 'ok/uapi/syscall.h'
            scheduler.write_text('user_init_pid = 1;\ncreate_init_process();\nuser_adoption_target();\n')
            uapi.write_text(UAPI_HEADER)
            write_pathname_and_directory_headers(headers)
            output = root / 'sdk'
            output.mkdir()
            previous = output / 'manifest.json'
            previous.write_text('{"existing":true}\n')
            args = argparse.Namespace(output=output, mode='release', define=[RUNTIME],
                                      asflag=[], cxxflag=[], arch='x86_64', headers=headers)
            for version in (46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 76):
                module.write_text(f'inline constexpr u32 kernel_module_abi_version = {version};\n')
                with self.subTest(version=version), self.assertRaisesRegex(ValueError, 'ABI75'):
                    export_sdk.export(args)
            module.write_text('inline constexpr u32 kernel_module_abi_version = 75;\n')
            uapi.write_text(UAPI_HEADER.replace('MINOR 16', 'MINOR 9'))
            with self.assertRaisesRegex(ValueError, 'UAPI 0.16.0'):
                export_sdk.export(args)
            uapi.write_text(UAPI_HEADER)
            for name in export_sdk.COMMON_HEADERS:
                path = headers / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(COMMON_HEADER if name.endswith('ext4_mounted_common_profile.hpp') else name)
            marker = headers / 'ok/fs/ext4_mounted_common_profile.hpp'
            for source in ('', COMMON_HEADER.replace('module_abi = 75', 'module_abi = 51'),
                           COMMON_HEADER.replace('module_abi = 75', 'module_abi = 52'),
                        COMMON_HEADER.replace('module_abi = 75', 'module_abi = 53'),
                        COMMON_HEADER.replace('module_abi = 75', 'module_abi = 54'),
                        COMMON_HEADER.replace('module_abi = 75', 'module_abi = 55'),
                        COMMON_HEADER.replace("module_abi = 75", "module_abi = 56"),
                           COMMON_HEADER.replace('counter_bits = 64', 'counter_bits = 32'),
                           COMMON_HEADER + COMMON_HEADER):
                marker.write_text(source)
                with self.subTest(common_profile=source), self.assertRaisesRegex(ValueError, 'common-opening'):
                    export_sdk.export(args)
            self.assertEqual(previous.read_text(), '{"existing":true}\n')
            self.assertEqual(sorted(path.name for path in root.iterdir()), ['headers', 'sdk'])

    def test_release_rejects_diagnostics(self):
        for defs in ([RUNTIME, TESTS], [RUNTIME, PRIVATE], [RUNTIME, PRIVATE, TESTS]):
            with self.subTest(defines=defs), self.assertRaises(ValueError):
                export_sdk.native_signal_profile('release', defs, [], 'x86_64')
        for flag in ('-D' + TESTS, '-D' + TESTS + '=1', '-D' + PRIVATE, '-D' + PRIVATE + '=1'):
            with self.subTest(flag=flag), self.assertRaises(ValueError):
                export_sdk.native_signal_profile('release', [RUNTIME], [flag], 'x86_64')

    def test_private_requires_complete_runtime_configuration(self):
        for defs in ([PRIVATE], [PRIVATE, TESTS], [PRIVATE, RUNTIME]):
            with self.subTest(defines=defs), self.assertRaises(ValueError):
                export_sdk.native_signal_profile('debug', defs, [], 'x86_64')

    def test_invalid_values_and_conflicts(self):
        for key in (RUNTIME, PRIVATE, TESTS):
            for value in ('0', 'false', '2', ''):
                for defs in ([key + '=' + value], [key, key + '=' + value]):
                    with self.subTest(defines=defs), self.assertRaises(ValueError):
                        export_sdk.native_signal_profile('debug', defs, [], 'x86_64')

    def test_mips_assembly_must_match_before_publication(self):
        for defs, flags in (([RUNTIME], []), ([], ['-D' + RUNTIME]), ([RUNTIME], ['-D' + RUNTIME + '=0'])):
            with self.subTest(defines=defs, flags=flags), self.assertRaises(ValueError):
                export_sdk.native_signal_profile('debug', defs, flags, 'mips64')

    def test_invalid_export_cannot_replace_existing_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'sdk'
            output.mkdir()
            previous = output / 'manifest.json'
            previous.write_text('{"existing":true}\n')
            digest = hashlib.sha256(previous.read_bytes()).hexdigest()
            args = argparse.Namespace(output=output, mode='release', define=[RUNTIME, TESTS], asflag=[], cxxflag=[], arch='x86_64')
            with self.assertRaises(ValueError):
                export_sdk.export(args)
            self.assertEqual(hashlib.sha256(previous.read_bytes()).hexdigest(), digest)
            self.assertEqual(sorted(p.name for p in output.parent.iterdir()), ['sdk'])


if __name__ == '__main__':
    unittest.main()
