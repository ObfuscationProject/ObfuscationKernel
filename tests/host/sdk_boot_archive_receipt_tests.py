"""Hosted exporter error controls; the real archive smoke is a separate gate."""
from pathlib import Path
import argparse
import contextlib
import importlib.util
import io
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from test_native_signal_export import UAPI_HEADER, TERMINATION_HEADER, write_pathname_and_directory_headers
from unittest.mock import patch


def load_exporter(path):
    spec = importlib.util.spec_from_file_location('receipt_exporter_under_test', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


TCP_HEADER = 'inline constexpr std::string_view native_tcp_profile = "bootcpu-static-peer-ipv4-captured-nonblocking-immediate-v1";\ninline constexpr std::string_view linux_tcp_profile = "bootcpu-direct-static-peer-ipv4-captured-nonblocking-immediate-v1";\ninline constexpr std::string_view native_tcp_accept_profile = "bootcpu-explicit-ipv4-captured-nonblocking-reserved-accept-v1";\ninline constexpr std::string_view linux_tcp_accept_profile = "bootcpu-direct-explicit-ipv4-captured-nonblocking-reserved-accept-v1";\ninline constexpr u32 native_tcp_module_abi = 76;\ninline constexpr usize native_tcp_stream_capacity = 16;\ninline constexpr usize native_tcp_connection_capacity = 8;\ninline constexpr usize native_tcp_listener_capacity = 4;\ninline constexpr usize native_tcp_backlog_capacity = 4;\ninline constexpr usize native_tcp_send_capacity = 1024;\ninline constexpr usize native_tcp_receive_capacity = 1024;\ninline constexpr usize native_tcp_segment_capacity = 536;\ninline constexpr usize native_tcp_poll_budget = 8;\n'


COMMON_HEADER = 'inline constexpr std::string_view ext4_common_opening_profile = "serialized-common-eight-family-directory-create-ordered-free-init-zero-refill-optional-revoke-legacy-v4";\ninline constexpr u32 ext4_common_module_abi = 76;\ninline constexpr u32 ext4_common_session_counter_bits = 64;\n'

UDP_HEADER = 'inline constexpr std::string_view native_udp_profile = "bootcpu-static-peer-ipv4-polling-captured-connected-zero-send-v3";\ninline constexpr std::string_view linux_udp_profile = "bootcpu-static-peer-ipv4-captured-connected-zero-send-v3";\ninline constexpr std::string_view native_udp_connected_profile = "bounded-static-peer-association-zero-destination-output-v2";\ninline constexpr u32 native_udp_module_abi = 76;\ninline constexpr usize native_udp_socket_capacity = 16;\ninline constexpr usize native_udp_queue_capacity = 2;\ninline constexpr usize native_udp_datagram_capacity = 1472;\ninline constexpr usize native_udp_frame_capacity = 1514;\n'

SHARED_FILE_HEADER = 'inline constexpr std::string_view native_shared_file_profile = "bootcpu-volatile-sealed-size-write-eager-pages-v3";\ninline constexpr std::string_view linux_shared_file_profile = "bootcpu-volatile-sealed-size-write-eager-pages-v3";\ninline constexpr u32 native_shared_file_module_abi = 76;\ninline constexpr usize native_shared_file_page_size = 4096;\ninline constexpr u32 native_shared_file_supported_seals = 15;\n'

UNIX_PATHNAME_HEADER = (
    'inline constexpr std::string_view native_unix_pathname_profile = "bootcpu-ram-pathname-stream-owned-queue-direct-v1";\n'
    'inline constexpr std::string_view linux_unix_pathname_profile = "bootcpu-ram-pathname-stream-owned-queue-direct-v1";\n'
    'inline constexpr u32 unix_pathname_module_abi = 76;\n'
    'inline constexpr usize unix_pathname_endpoint_capacity = 32;\n'
    'inline constexpr usize unix_pathname_backlog_capacity = 4;\n'
    'inline constexpr usize unix_pathname_address_capacity = 110;\n'
    'inline constexpr usize unix_pathname_text_capacity = 107;\n'
    'inline constexpr usize unix_pathname_peer_credentials_bytes = 12;\n'
)

DIRECTORY_CREATE_HEADER = (
    'inline constexpr std::string_view ext4_directory_create_profile = "bounded-journalled-single-block-directory-create-ordered-free-init-v2";\n'
    'inline constexpr u32 ext4_directory_create_module_abi = 76;\n'
    'inline constexpr usize ext4_directory_create_home_capacity = 8;\n'
    'inline constexpr usize ext4_directory_create_owner_buffers = 9;\n'
)

class ReceiptTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='sdk-receipt-model-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.stage = self.root / 'stage'
        (self.stage / 'lib').mkdir(parents=True)
        self.tool = self.root / 'ar'
        self.tool.write_bytes(b'model archive tool')
        self.tool.chmod(0o755)
        self.objects = [self.root / 'first.o', self.root / 'second.o']
        for ordinal, obj in enumerate(self.objects):
            obj.write_bytes(b'model object ' + bytes([ordinal]))
        self.args = argparse.Namespace(ar=self.tool, boot_object=self.objects,
                                       output=self.root / 'sdk', arch='i386', mode='debug')
        self.invocations = []

    def ar_success(self, argv, *, cwd, check):
        self.invocations.append({'argv': list(argv), 'cwd': str(cwd), 'check': check})
        Path(argv[2]).write_bytes(b'model archive')
        return subprocess.CompletedProcess(argv, 0)

    def construct(self, side_effect=None):
        with patch.object(EXPORTER.subprocess, 'run', side_effect=side_effect or self.ar_success):
            return EXPORTER.construct_boot_archive(self.args, self.stage)

    def test_exact_ordered_invocation(self):
        receipt = self.construct()
        argv = [str(self.tool.resolve()), 'rcs', str(self.stage / 'lib/libokernel_boot.a'),
                *map(str, self.objects)]
        self.assertEqual(self.invocations, [{'argv': argv, 'cwd': str(Path.cwd().resolve()), 'check': True}])
        self.assertEqual(receipt['argv'], argv)
        self.assertEqual([m['ordinal'] for m in receipt['members']], [0, 1])
        self.assertEqual([m['object']['path'] for m in receipt['members']], list(map(str, self.objects)))
        self.assertEqual(receipt['returncode'], 0)
        self.assertNotIn('published_archive', receipt)

    def test_no_objects_refuses_before_invocation(self):
        self.args.boot_object = []
        with self.assertRaisesRegex(ValueError, 'explicit producing objects'):
            self.construct()
        self.assertEqual(self.invocations, [])

    def test_missing_object_refuses_before_invocation(self):
        self.objects[1].unlink()
        with self.assertRaises(FileNotFoundError):
            self.construct()
        self.assertEqual(self.invocations, [])

    def test_nonregular_object_refuses_before_invocation(self):
        self.objects[1].unlink()
        self.objects[1].mkdir()
        with self.assertRaisesRegex(ValueError, 'not regular'):
            self.construct()
        self.assertEqual(self.invocations, [])

    def test_preexisting_staged_output_refuses_before_invocation(self):
        (self.stage / 'lib/libokernel_boot.a').write_bytes(b'previous')
        with self.assertRaisesRegex(ValueError, 'already exists'):
            self.construct()
        self.assertEqual(self.invocations, [])

    def test_ar_failure_does_not_complete_receipt(self):
        def failing(argv, **kwargs):
            self.ar_success(argv, **kwargs)
            raise subprocess.CalledProcessError(4, argv)
        with self.assertRaises(subprocess.CalledProcessError):
            self.construct(failing)
        self.assertEqual(len(self.invocations), 1)

    def test_missing_ar_output_refuses(self):
        with self.assertRaises(FileNotFoundError):
            self.construct(lambda *args, **kwargs: None)

    def test_changed_object_after_ar_refuses(self):
        def mutating(argv, **kwargs):
            self.ar_success(argv, **kwargs)
            self.objects[0].write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'input changed'):
            self.construct(mutating)

    def test_changed_tool_mode_after_ar_refuses(self):
        def mutating(argv, **kwargs):
            self.ar_success(argv, **kwargs)
            self.tool.chmod(0o700)
        with self.assertRaisesRegex(ValueError, 'input changed'):
            self.construct(mutating)

    def test_published_path_does_not_replace_staging_argv(self):
        receipt = self.construct()
        self.stage.rename(self.args.output)
        completed = EXPORTER.finish_boot_archive_receipt(receipt, self.args.output)
        self.assertEqual(completed['argv'], receipt['argv'])
        self.assertFalse(Path(completed['argv'][2]).exists())
        self.assertEqual(completed['published_archive']['path'], str(self.args.output / 'lib/libokernel_boot.a'))
        self.assertEqual(completed['published_archive']['entry'], completed['staged_archive']['entry'])

    def test_wrong_publication_directory_refuses(self):
        receipt = self.construct()
        other = self.root / 'other'
        self.stage.rename(other)
        with self.assertRaisesRegex(ValueError, 'directory changed'):
            EXPORTER.finish_boot_archive_receipt(receipt, other)

    def test_changed_published_bytes_refuse(self):
        receipt = self.construct()
        self.stage.rename(self.args.output)
        (self.args.output / 'lib/libokernel_boot.a').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'changed during directory publication'):
            EXPORTER.finish_boot_archive_receipt(receipt, self.args.output)

    def test_changed_published_mode_refuses(self):
        receipt = self.construct()
        self.stage.rename(self.args.output)
        (self.args.output / 'lib/libokernel_boot.a').chmod(0o600)
        with self.assertRaisesRegex(ValueError, 'changed during directory publication'):
            EXPORTER.finish_boot_archive_receipt(receipt, self.args.output)

    def test_changed_input_before_publication_refuses(self):
        receipt = self.construct()
        self.stage.rename(self.args.output)
        self.objects[1].write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'input changed'):
            EXPORTER.finish_boot_archive_receipt(receipt, self.args.output)

    def configure_export(self):
        headers = self.root / 'headers'
        declarations = {
            'ok/core/module.hpp': 'inline constexpr u32 kernel_module_abi_version = 76;\n',
            'ok/sched/scheduler.hpp': 'user_init_pid = 1; create_init_process(); user_adoption_target();',
            'ok/uapi/syscall.h': UAPI_HEADER,
            EXPORTER.COMMON_HEADERS[0]: COMMON_HEADER,
            EXPORTER.UDP_PROFILE_HEADER: UDP_HEADER,
        }
        declarations[EXPORTER.TCP_PROFILE_HEADER] = TCP_HEADER
        declarations[EXPORTER.SHARED_FILE_PROFILE_HEADER] = SHARED_FILE_HEADER
        declarations[EXPORTER.UNIX_PATHNAME_PROFILE_HEADER] = UNIX_PATHNAME_HEADER
        declarations[EXPORTER.DIRECTORY_CREATE_PROFILE_HEADER] = DIRECTORY_CREATE_HEADER
        declarations[EXPORTER.FUTEX_WAKE_OPERATION_PROFILE_HEADER] = 'inline constexpr std::string_view native_futex_wake_operation_profile = "bootcpu-serialized-two-key-private-anonymous-shared-signed-wrap-legacy-count-v1";\ninline constexpr std::string_view linux_futex_wake_operation_profile = "bootcpu-serialized-two-key-private-anonymous-shared-signed-wrap-legacy-count-v1";\ninline constexpr u32 native_futex_wake_operation_module_abi = 76;\ninline constexpr usize native_futex_wake_operation_word_bits = 32;\ninline constexpr usize native_futex_wake_operation_operand_bits = 12;\ninline constexpr usize native_futex_wake_operation_shift_bits = 5;\ninline constexpr usize native_futex_wake_operation_arithmetic_count = 5;\ninline constexpr usize native_futex_wake_operation_comparison_count = 6;\ninline constexpr usize native_futex_wake_operation_wake_passes = 2;\ninline constexpr usize native_futex_wake_operation_key_domains = 2;\ninline constexpr usize native_futex_wake_operation_nonpositive_wake_minimum = 1;\ninline constexpr u32 native_futex_wake_operation_smp_atomic = 0;\ninline constexpr u32 native_futex_wake_operation_file_backed = 0;\ninline constexpr u32 native_futex_wake_operation_cross_process_clone_vm = 0;\n'
        declarations[EXPORTER.FUTEX_REQUEUE_PROFILE_HEADER] = 'inline constexpr std::string_view native_futex_requeue_profile = "bootcpu-nonpi-read-keys-signed-counts-exact-origin-ticket-sequence-owned-transfer-v1";\ninline constexpr std::string_view linux_futex_requeue_profile = "bootcpu-nonpi-read-keys-signed-counts-exact-origin-ticket-sequence-owned-transfer-v1";\ninline constexpr u32 native_futex_requeue_module_abi = 76;\ninline constexpr usize native_futex_requeue_word_bits = 32;\ninline constexpr usize native_futex_requeue_operation_count = 2;\ninline constexpr usize native_futex_requeue_linux_operation = 3;\ninline constexpr usize native_futex_cmp_requeue_linux_operation = 4;\ninline constexpr usize native_futex_requeue_private_flag = 128;\ninline constexpr usize native_futex_requeue_key_domains = 2;\ninline constexpr usize native_futex_requeue_waiter_capacity = 64;\ninline constexpr usize native_futex_requeue_sequence_bits = 64;\ninline constexpr u32 native_futex_requeue_original_continuation_preserved = 1;\ninline constexpr u32 native_futex_requeue_same_key_sequence_preserved = 1;\ninline constexpr u32 native_futex_requeue_zero_wake_minimum = 0;\ninline constexpr u32 native_futex_requeue_signed_negative_counts_rejected = 1;\ninline constexpr u32 native_futex_requeue_transfer_pin_owners_retained = 1;\ninline constexpr u32 native_futex_requeue_smp_atomic = 0;\ninline constexpr u32 native_futex_requeue_priority_inheritance = 0;\ninline constexpr u32 native_futex_requeue_realtime_priority = 0;\ninline constexpr u32 native_futex_requeue_file_backed = 0;\ninline constexpr u32 native_futex_requeue_cross_process_clone_vm = 0;\n'
        for name in set(EXPORTER.COMMON_HEADERS + EXPORTER.UDP_PROFILE_HEADERS + EXPORTER.TCP_PROFILE_HEADERS + EXPORTER.SHARED_FILE_PROFILE_HEADERS + EXPORTER.UNIX_PATHNAME_PROFILE_HEADERS + EXPORTER.DIRECTORY_CREATE_PROFILE_HEADERS + EXPORTER.FUTEX_WAKE_OPERATION_PROFILE_HEADERS + EXPORTER.FUTEX_REQUEUE_PROFILE_HEADERS) | set(declarations):
            target = headers / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(declarations.get(name, '// hosted exporter fixture\n'))
        write_pathname_and_directory_headers(headers)
        (headers / 'ok/uapi/signal.h').write_text(TERMINATION_HEADER)
        self.args.headers = headers
        for name in ('library', 'linker_script', 'kernel_bin', 'kernel_elf', 'compiler'):
            path = self.root / name
            path.write_bytes(b'hosted export fixture')
            setattr(self.args, name, path)
        self.args.define = []; self.args.asflag = []; self.args.cxxflag = []; self.args.ldflag = []
        self.args.version = '0.1.0'; self.args.image_format = 'elf'; self.args.triple = 'model-elf'

    def full_export(self, finish=None, rename=None):
        def query(argv, **kwargs):
            return 'model-elf\n' if argv[-1] == '-dumpmachine' else '1.0\n'
        with contextlib.ExitStack() as stack:
            stack.enter_context(patch.object(EXPORTER.subprocess, 'run', side_effect=self.ar_success))
            stack.enter_context(patch.object(EXPORTER.subprocess, 'check_output', side_effect=query))
            if finish is not None:
                stack.enter_context(patch.object(EXPORTER, 'finish_boot_archive_receipt', side_effect=finish))
            if rename is not None:
                stack.enter_context(patch.object(Path, 'rename', new=rename))
            EXPORTER.export(self.args)

    def old_output(self):
        self.args.output.mkdir()
        (self.args.output / 'prior-sdk').write_bytes(b'preserved prior SDK')

    def test_complete_export_prints_one_canonical_receipt(self):
        self.configure_export(); self.old_output()
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.full_export()
        lines = output.getvalue().splitlines()
        records = [line[len(EXPORTER.BOOT_ARCHIVE_RECEIPT_PREFIX):] for line in lines
                   if line.startswith(EXPORTER.BOOT_ARCHIVE_RECEIPT_PREFIX)]
        self.assertEqual(len(records), 1)
        record = json.loads(records[0])
        self.assertEqual(records[0], json.dumps(record, sort_keys=True, separators=(',', ':')))
        self.assertTrue((self.args.output / 'manifest.json').is_file())
        self.assertFalse(self.args.output.with_name('sdk.previous').exists())

    def test_postpublication_failure_restores_previous_sdk_and_prints_no_receipt(self):
        self.configure_export(); self.old_output()
        output = io.StringIO()
        with contextlib.redirect_stdout(output), self.assertRaisesRegex(ValueError, 'injected publication failure'):
            self.full_export(finish=lambda *args: (_ for _ in ()).throw(ValueError('injected publication failure')))
        self.assertEqual(output.getvalue(), '')
        self.assertEqual(list(self.args.output.iterdir()), [self.args.output / 'prior-sdk'])
        self.assertEqual((self.args.output / 'prior-sdk').read_bytes(), b'preserved prior SDK')

    def test_postpublication_failure_without_previous_sdk_leaves_no_public_output(self):
        self.configure_export()
        with self.assertRaisesRegex(ValueError, 'injected publication failure'):
            self.full_export(finish=lambda *args: (_ for _ in ()).throw(ValueError('injected publication failure')))
        self.assertFalse(self.args.output.exists())

    def test_stage_rename_failure_restores_previous_sdk(self):
        self.configure_export(); self.old_output()
        original = Path.rename
        def failing(path, target):
            if path.name.startswith('.kernel-sdk-'):
                raise OSError('injected rename failure')
            return original(path, target)
        with self.assertRaisesRegex(OSError, 'injected rename failure'):
            self.full_export(rename=failing)
        self.assertEqual((self.args.output / 'prior-sdk').read_bytes(), b'preserved prior SDK')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exporter', type=Path, required=True)
    args = parser.parse_args()
    EXPORTER = load_exporter(args.exporter.resolve())
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ReceiptTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if result.wasSuccessful():
        print(f'SDK_BOOT_ARCHIVE_RECEIPT_MODEL_PASS tests={result.testsRun} actual_ar=0 actual_compile=0')
    sys.exit(not result.wasSuccessful())
