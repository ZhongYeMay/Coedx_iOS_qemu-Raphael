"""Tests use synthetic firmware and fake QEMU; no Apple firmware is executed."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import warnings
import zipfile

MODULE = Path(__file__).resolve().parents[2] / 'tools/ios27/ios27.py'
spec = importlib.util.spec_from_file_location('ios27', MODULE)
ios27 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ios27)
REAL_PROBE = ios27.probe


class PreflightTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='ios27 test ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.firmware = self.root / 'firmware with spaces'
        self.firmware.mkdir()
        for name in ios27.REQUIRED:
            (self.firmware / name).write_bytes(b'SYNTHETIC TEST ONLY')
        self.qemu = self.root / 'fake qemu'
        self.qemu.write_text('#!' + sys.executable + '\n' + '''import sys, time
args = sys.argv[1:]
if args == ['-machine', 'help']:
    print('Supported machines are:\\ndarwin  synthetic fixture')
elif args == ['-accel', 'help']:
    print('Accelerators supported in QEMU binary:\\ntcg')
elif args == ['-help']:
    print('\\n'.join('-' + x + ' file' for x in ['bootkc', 'dtree', 'tc', 'ramdisk', 'args', 'sptm', 'txm']))
elif args == ['--version']:
    print('FAKE QEMU: unit test fixture, not an emulator')
else:
    print('synthetic run only', flush=True)
    time.sleep(10 if 'SLEEP' in args else 0)
''')
        self.qemu.chmod(0o755)
        def fixture_probe(qemu, *args):
            return {
                ('-machine', 'help'): 'Supported machines are:\ndarwin synthetic fixture',
                ('-accel', 'help'): 'Accelerators supported in QEMU binary:\ntcg',
                ('-help',): '\n'.join('-' + n + ' file' for n in ('bootkc', 'dtree', 'tc', 'ramdisk', 'args', 'sptm', 'txm')),
                ('--version',): 'FAKE QEMU: unit test fixture, not an emulator',
            }[args]
        patcher = mock.patch.object(ios27, 'probe', side_effect=fixture_probe)
        patcher.start()
        self.addCleanup(patcher.stop)

    def check(self, **kwargs):
        return ios27.preflight(self.firmware, str(self.qemu), **kwargs)

    def test_complete_non_sptm_fixture(self):
        report = self.check()
        self.assertTrue(report['ready_for_attempt'], report['errors'])
        self.assertFalse(report['boot_verified'])
        self.assertFalse(report['ios_version_verified'])
        self.assertIn(str(self.firmware / 'bootkc'), report['argv'])
        self.assertEqual(report['argv'][report['argv'].index('-accel') + 1], 'tcg')

    def test_missing_each_required_file(self):
        for name in ios27.REQUIRED:
            with self.subTest(name=name):
                path = self.firmware / name
                path.unlink()
                self.assertFalse(self.check()['ready_for_attempt'])
                path.write_bytes(b'fixture')

    def test_empty_required_file(self):
        (self.firmware / 'ramdisk.tc').write_bytes(b'')
        self.assertFalse(self.check()['ready_for_attempt'])

    def test_sptm_without_txm(self):
        (self.firmware / 'sptm').write_bytes(b'fixture')
        self.assertFalse(self.check()['ready_for_attempt'])

    def test_txm_without_sptm(self):
        (self.firmware / 'txm').write_bytes(b'fixture')
        self.assertFalse(self.check()['ready_for_attempt'])

    def test_complete_sptm_pair(self):
        for name in ('sptm', 'txm'):
            (self.firmware / name).write_bytes(b'fixture')
        self.assertIn('-txm', self.check()['argv'])

    def test_empty_sptm(self):
        (self.firmware / 'sptm').touch()
        (self.firmware / 'txm').write_bytes(b'fixture')
        self.assertFalse(self.check()['ready_for_attempt'])

    def test_stock_qemu_rejected(self):
        original = ios27.probe
        def probe(qemu, *args):
            return 'virt ARM Virtual Machine\n' if args == ('-machine', 'help') else original(qemu, *args)
        with mock.patch.object(ios27, 'probe', side_effect=probe):
            self.assertFalse(self.check()['ready_for_attempt'])

    def test_missing_tcg(self):
        original = ios27.probe
        def probe(qemu, *args):
            return 'kvm\n' if args == ('-accel', 'help') else original(qemu, *args)
        with mock.patch.object(ios27, 'probe', side_effect=probe):
            self.assertFalse(self.check()['ready_for_attempt'])

    def test_custom_option_boundary(self):
        original = ios27.probe
        def probe(qemu, *args):
            text = original(qemu, *args)
            return text.replace('-tc file', '-tc-extra file') if args == ('-help',) else text
        with mock.patch.object(ios27, 'probe', side_effect=probe):
            self.assertFalse(self.check()['ready_for_attempt'])

    def test_probe_timeout(self):
        with mock.patch.object(ios27, 'probe', side_effect=subprocess.TimeoutExpired('fixture', 10)):
            self.assertFalse(self.check()['ready_for_attempt'])

    def test_missing_qemu(self):
        report = ios27.preflight(self.firmware, str(self.root / 'missing'))
        self.assertFalse(report['ready_for_attempt'])
        self.assertEqual(report['argv'], [])

    def test_non_executable_qemu(self):
        self.qemu.chmod(0o644)
        self.assertFalse(self.check()['ready_for_attempt'])

    def test_invalid_memory(self):
        for value in ('0G', '-1G', '8G;echo injected', 'NaN', '8'):
            with self.subTest(value=value):
                self.assertFalse(self.check(memory=value)['ready_for_attempt'])

    def test_refuse_failed_preflight(self):
        with self.assertRaises(ValueError):
            ios27.run_attempt({'ready_for_attempt': False}, self.root / 'log', 1)

    def test_fake_foreground_run_is_not_boot_proof(self):
        log = self.root / 'run.log'
        result = ios27.run_attempt(self.check(), log, 2)
        self.assertEqual(result['state'], 'process_exited')
        self.assertEqual(result['returncode'], 0)
        self.assertFalse(result['boot_verified'])
        self.assertIn('synthetic run only', log.read_text())

    def test_timeout_reaps_child(self):
        report = self.check()
        report['argv'].append('SLEEP')
        result = ios27.run_attempt(report, self.root / 'timeout.log', 1)
        self.assertEqual(result['state'], 'timeout')
        self.assertIsInstance(result['returncode'], int)
        self.assertFalse(result['boot_verified'])

    def test_log_never_overwritten(self):
        log = self.root / 'existing.log'
        log.write_bytes(b'preserve')
        with self.assertRaises(FileExistsError):
            ios27.run_attempt(self.check(), log, 1)
        self.assertEqual(log.read_bytes(), b'preserve')

    def test_invalid_timeout(self):
        with self.assertRaises(ValueError):
            ios27.run_attempt(self.check(), self.root / 'log', 0)

    def test_cli_missing_firmware_reports_failure(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = ios27.main(['check', '--qemu', str(self.qemu), '--firmware-dir', str(self.root / 'missing')])
        self.assertEqual(code, 2)
        self.assertFalse(json.loads(output.getvalue())['ready_for_attempt'])

    def test_real_subprocess_capability_probe_on_fixture(self):
        with mock.patch.object(ios27, 'probe', side_effect=REAL_PROBE):
            report = self.check()
        self.assertTrue(report['ready_for_attempt'], report['errors'])
        self.assertIn('FAKE QEMU', report['qemu_version'])


class IpswTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'fixture.ipsw'
        self.manifest = {'ProductVersion': '27.0', 'ProductBuildVersion': 'TEST-ONLY',
                         'SupportedProductTypes': ['TestDevice1,1'],
                         'BuildIdentities': [{'Info': {'DeviceClass': 'testap', 'Variant': 'test'}}]}

    def write(self, manifest=None, name='BuildManifest.plist'):
        with zipfile.ZipFile(self.path, 'w') as archive:
            archive.writestr(name, plistlib.dumps(self.manifest if manifest is None else manifest))

    def test_inspect_metadata(self):
        self.write()
        report = ios27.inspect_ipsw(self.path)
        self.assertEqual(report['product_version'], '27.0')
        self.assertEqual(report['identities'][0]['board'], 'testap')
        self.assertIn('metadata only', report['verification'])

    def test_wrong_version(self):
        self.manifest['ProductVersion'] = '26.6'
        self.write()
        with self.assertRaises(ValueError):
            ios27.inspect_ipsw(self.path)

    def test_missing_manifest(self):
        self.write(name='nested/BuildManifest.plist')
        with self.assertRaises(ValueError):
            ios27.inspect_ipsw(self.path)

    def test_duplicate_manifest(self):
        self.write()
        with warnings.catch_warnings():
            warnings.simplefilter('ignore', UserWarning)
            with zipfile.ZipFile(self.path, 'a') as archive:
                archive.writestr('BuildManifest.plist', plistlib.dumps(self.manifest))
        with self.assertRaises(ValueError):
            ios27.inspect_ipsw(self.path)

    def test_bounded_manifest(self):
        self.write()
        with mock.patch.object(ios27, 'MANIFEST_LIMIT', 8):
            with self.assertRaises(ValueError):
                ios27.inspect_ipsw(self.path)

    def test_malformed_metadata(self):
        for field, value in [('ProductVersion', 27), ('ProductBuildVersion', ''),
                             ('SupportedProductTypes', 'wrong'), ('BuildIdentities', [])]:
            with self.subTest(field=field):
                manifest = dict(self.manifest)
                manifest[field] = value
                self.write(manifest)
                with self.assertRaises(ValueError):
                    ios27.inspect_ipsw(self.path)

    def test_no_archive_extraction(self):
        self.write()
        with zipfile.ZipFile(self.path, 'a') as archive:
            archive.writestr('../escape.txt', 'must not be extracted')
        ios27.inspect_ipsw(self.path)
        self.assertFalse((self.path.parent.parent / 'escape.txt').exists())

    def test_corrupt_archive_cli(self):
        self.path.write_bytes(b'not a zip')
        with contextlib.redirect_stderr(io.StringIO()) as output:
            code = ios27.main(['inspect-ipsw', str(self.path)])
        self.assertEqual(code, 2)
        self.assertIn('error', json.loads(output.getvalue()))

    def test_corrupt_xml_cli(self):
        with zipfile.ZipFile(self.path, 'w') as archive:
            archive.writestr('BuildManifest.plist', '<?xml version="1.0"?><plist><dict>')
        with contextlib.redirect_stderr(io.StringIO()) as output:
            code = ios27.main(['inspect-ipsw', str(self.path)])
        self.assertEqual(code, 2)
        self.assertIn('error', json.loads(output.getvalue()))


if __name__ == '__main__':
    unittest.main()
