#!/usr/bin/env python3
"""Offline IPSW inspection and guarded qemu-sptm launch. Python 3.10+, stdlib only."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import signal
import subprocess
import sys
import zipfile
from xml.parsers.expat import ExpatError

REQUIRED = ('bootkc', 'dtree', 'ramdisk.tc', 'ramdisk.dmg')
BOOT_ARGS = 'rd=md0 serial=3 -v -noprogress wdt=-1 wlan-olyhal-abort'
MANIFEST_LIMIT = 16 * 1024 * 1024


def inspect_ipsw(path: Path, major: int = 27) -> dict:
    """Read one bounded manifest; never extract or execute archive members."""
    with zipfile.ZipFile(path) as archive:
        entries = [i for i in archive.infolist() if i.filename == 'BuildManifest.plist']
        if len(entries) != 1:
            raise ValueError('IPSW must contain exactly one root BuildManifest.plist')
        if entries[0].file_size > MANIFEST_LIMIT:
            raise ValueError('BuildManifest.plist exceeds 16 MiB limit')
        with archive.open(entries[0]) as stream:
            raw = stream.read(MANIFEST_LIMIT + 1)
        if len(raw) > MANIFEST_LIMIT:
            raise ValueError('BuildManifest.plist exceeds 16 MiB limit')
    try:
        manifest = plistlib.loads(raw)
    except (plistlib.InvalidFileException, ExpatError, ValueError, OverflowError) as exc:
        raise ValueError(f'Malformed BuildManifest.plist: {exc}') from exc
    if not isinstance(manifest, dict):
        raise ValueError('BuildManifest.plist must be a dictionary')
    version = manifest.get('ProductVersion')
    build = manifest.get('ProductBuildVersion')
    devices = manifest.get('SupportedProductTypes')
    identities = manifest.get('BuildIdentities')
    if not isinstance(version, str) or not re.fullmatch(r'\d+(?:\.\d+){0,3}', version):
        raise ValueError('Missing or malformed ProductVersion')
    if int(version.split('.')[0]) != major:
        raise ValueError(f'Expected iOS major {major}; manifest declares {version}')
    if not isinstance(build, str) or not build.strip():
        raise ValueError('Missing ProductBuildVersion')
    if not isinstance(devices, list) or not devices or not all(isinstance(d, str) and d for d in devices):
        raise ValueError('Missing or malformed SupportedProductTypes')
    if not isinstance(identities, list) or not identities:
        raise ValueError('Missing BuildIdentities')
    result = []
    for index, identity in enumerate(identities):
        if not isinstance(identity, dict) or not isinstance(identity.get('Info'), dict):
            raise ValueError(f'Malformed BuildIdentity {index}')
        info = identity['Info']
        result.append({'index': index, 'board': info.get('DeviceClass'),
                       'variant': info.get('Variant')})
    return {'product_version': version, 'product_build': build,
            'supported_devices': devices, 'identities': result,
            'verification': 'manifest metadata only; not signature or boot verification'}


def resolve_qemu(value: str) -> str:
    if '/' in value or '\\' in value:
        path = Path(value).expanduser().resolve()
        if not path.is_file() or not os.access(path, os.X_OK):
            raise ValueError(f'QEMU is missing or not executable: {path}')
        return str(path)
    found = shutil.which(value)
    if not found:
        raise ValueError(f'QEMU not found in PATH: {value}')
    return str(Path(found).resolve())


def probe(qemu: str, *args: str) -> str:
    result = subprocess.run([qemu, *args], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True,
                            errors='replace', timeout=10, check=False)
    if result.returncode:
        raise ValueError(f'QEMU probe {args!r} failed ({result.returncode}): {result.stdout[:500]}')
    return result.stdout


def preflight(firmware: Path, qemu: str, memory: str = '8G') -> dict:
    firmware = firmware.expanduser().resolve()
    errors: list[str] = []
    warnings = [
        'Preflight does not verify firmware signatures, image compatibility or guest boot.',
        'Linux does not replace upstream macOS ramdisk patching; use a prepared ramdisk and matching trustcache.',
        'The selected QEMU executable is run for capability probes; use a trusted build.'
    ]
    if not re.fullmatch(r'[1-9][0-9]*[MG]', memory):
        errors.append('Memory must be a positive integer followed by M or G (for example 8G)')
    for name in REQUIRED:
        path = firmware / name
        try:
            if not path.is_file() or path.stat().st_size == 0:
                errors.append(f'Missing or empty required firmware: {name}')
            else:
                with path.open('rb') as stream:
                    stream.read(1)
        except OSError as exc:
            errors.append(f'Unreadable firmware {name}: {exc}')
    optional = [(firmware / n).exists() for n in ('sptm', 'txm')]
    if any(optional) and not all(optional):
        errors.append('SPTM and TXM must be supplied together, or both omitted for a compatible non-SPTM guest')
    for name, present in zip(('sptm', 'txm'), optional):
        if present:
            try:
                path = firmware / name
                if not path.is_file() or path.stat().st_size == 0:
                    errors.append(f'Empty or invalid optional firmware: {name}')
                else:
                    with path.open('rb') as stream:
                        stream.read(1)
            except OSError as exc:
                errors.append(f'Unreadable firmware {name}: {exc}')
    executable = None
    version = None
    try:
        executable = resolve_qemu(qemu)
        if not re.search(r'^\s*darwin(?:\s|$)', probe(executable, '-machine', 'help'), re.M):
            errors.append('QEMU does not advertise the required darwin machine; stock virt is not a substitute')
        if not re.search(r'^\s*tcg\s*$', probe(executable, '-accel', 'help'), re.M):
            errors.append('QEMU does not advertise TCG acceleration')
        help_text = probe(executable, '-help')
        options = ('bootkc', 'dtree', 'tc', 'ramdisk', 'args')
        if any(optional):
            options += ('sptm', 'txm')
        for option in options:
            if not re.search(r'(?m)^\s*-' + option + r'(?:\s|$)', help_text):
                errors.append(f'QEMU is missing required custom option: -{option}')
        version = probe(executable, '--version').splitlines()[0]
    except (OSError, ValueError, subprocess.SubprocessError, IndexError) as exc:
        errors.append(str(exc))
    argv = []
    if not errors:
        argv = [executable, '-M', 'darwin', '-accel', 'tcg',
                '-bootkc', str(firmware / 'bootkc'), '-dtree', str(firmware / 'dtree'),
                '-tc', str(firmware / 'ramdisk.tc'), '-ramdisk', str(firmware / 'ramdisk.dmg'),
                '-args', BOOT_ARGS, '-display', 'none', '-monitor', 'none',
                '-serial', 'stdio', '-m', memory]
        if all(optional):
            argv += ['-sptm', str(firmware / 'sptm'), '-txm', str(firmware / 'txm')]
    return {'schema_version': 1, 'ready_for_attempt': not errors,
            'boot_verified': False, 'ios_version_verified': False,
            'host': {'system': sys.platform, 'kvm_present': Path('/dev/kvm').exists()},
            'qemu_version': version, 'firmware_dir': str(firmware),
            'errors': errors, 'warnings': warnings, 'argv': argv}


def stop_process(proc: subprocess.Popen) -> None:
    """Reap the bounded foreground attempt, including its process group."""
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        proc.wait()


def run_attempt(report: dict, log: Path, timeout: int) -> dict:
    if not report['ready_for_attempt']:
        raise ValueError('Refusing launch: preflight failed')
    if not 1 <= timeout <= 3600:
        raise ValueError('Timeout must be between 1 and 3600 seconds')
    if os.name != 'posix':
        raise ValueError('Bounded launch currently supports Linux/macOS only')
    # Exclusive creation protects an existing log or an accidentally selected firmware file.
    with log.open('xb') as stream:
        proc = subprocess.Popen(report['argv'], stdin=subprocess.DEVNULL,
                                stdout=stream, stderr=subprocess.STDOUT,
                                start_new_session=True)
        state = 'process_exited'
        try:
            proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            state = 'timeout'
            stop_process(proc)
        except KeyboardInterrupt:
            state = 'interrupted'
            stop_process(proc)
    return {'state': state, 'returncode': proc.returncode,
            'log': str(log.resolve()), 'boot_verified': False}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    inspect = commands.add_parser('inspect-ipsw', help='Read local IPSW metadata without extraction')
    inspect.add_argument('ipsw', type=Path)
    inspect.add_argument('--expected-major', type=int, default=27)
    for name in ('check', 'run'):
        sub = commands.add_parser(name)
        sub.add_argument('--firmware-dir', type=Path, default=Path('firmware'))
        sub.add_argument('--qemu', default='qemu-sptm/build/qemu-system-aarch64')
        sub.add_argument('--memory', default='8G')
        if name == 'run':
            sub.add_argument('--log', required=True, type=Path)
            sub.add_argument('--timeout', type=int, default=120)
    args = parser.parse_args(argv)
    try:
        if args.command == 'inspect-ipsw':
            result = inspect_ipsw(args.ipsw, args.expected_major)
            code = 0
        else:
            result = preflight(args.firmware_dir, args.qemu, args.memory)
            code = 0 if result['ready_for_attempt'] else 2
            if args.command == 'run' and not code:
                result['attempt'] = run_attempt(result, args.log, args.timeout)
                attempt = result['attempt']
                code = (124 if attempt['state'] == 'timeout' else
                        130 if attempt['state'] == 'interrupted' else
                        0 if attempt['returncode'] == 0 else 1)
        print(json.dumps(result, indent=2))
        return code
    except (OSError, ValueError, zipfile.BadZipFile, plistlib.InvalidFileException,
            RuntimeError, EOFError) as exc:
        print(json.dumps({'error': str(exc), 'boot_verified': False}), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
