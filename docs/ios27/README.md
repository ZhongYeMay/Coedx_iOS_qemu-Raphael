# iOS 27 bring-up: inspection and guarded launch

Experimental, additive tooling; Python 3.10+, standard library only. This does not
change Raphael's existing XNU, VirtIO-GPU or iOS 16.5 paths. No iOS image is included.

## Commands

From the repository root:

```sh
python3 tools/ios27/ios27.py inspect-ipsw /path/to/legally-obtained.ipsw
python3 tools/ios27/ios27.py check \
  --qemu /path/to/qemu-sptm/build/qemu-system-aarch64 \
  --firmware-dir /path/to/prepared-firmware > preflight.json
python3 tools/ios27/ios27.py run \
  --qemu /path/to/qemu-sptm/build/qemu-system-aarch64 \
  --firmware-dir /path/to/prepared-firmware \
  --timeout 120 --log boot-attempt.log > attempt.json
python3 -m unittest discover -s tests/ios27 -v
```

`inspect-ipsw` reads one bounded, root-level BuildManifest.plist without extracting
archive members. It reports the declared version/build/device/board identities.
The expected major version defaults to 27; override explicitly with `--expected-major`.

`check` requires readable, nonempty bootkc, dtree, ramdisk.tc and ramdisk.dmg. It
executes a trusted QEMU binary's help commands to check for the custom `darwin`
machine, TCG and the required CLI options. Stock QEMU `virt` is not a substitute.
SPTM and TXM must either both be present or both be omitted for a compatible guest.

`run` uses the checked argument array without shell evaluation, runs in the
foreground with a timeout, captures serial output and terminates/reaps QEMU on
timeout or interruption. An existing log is never overwritten. This command is
noninteractive: stdin is closed. Use the checked argv in a terminal for interaction.
Linux/macOS are supported for the process-management path; only Linux was tested.

## Exit codes and limits

`check`: 0 permits an attempt, 2 means prerequisites failed. `run`: 124 is timeout,
130 is interrupted, 1 is a nonzero process exit, 2 is validation/I/O failure.
**Exit 0 is not proof that iOS booted.** Every report retains `boot_verified: false`.

Manifest inspection does not authenticate Apple signatures or associate separate
prepared files with that IPSW. Preflight does not validate Mach-O/IMG4/device-tree,
trustcache or ramdisk contents, matching builds, SPTM requirements, host capacity
or usable emulation performance. Use only a trusted QEMU executable for probes.
The 8G default follows upstream, not the host's memory limit; adjust `--memory`
after checking both the host/container limits and the guest's requirements.

The upstream ramdisk patching flow requires macOS tools. This increment does NOT
implement a Linux replacement: a matching prepared ramdisk/trustcache is still
required. No actual QEMU compilation or iOS 27 boot was performed for this release.
Tests use synthetic firmware and fake QEMU fixtures, including real subprocess
probes and lifecycle tests against that fixture, not an emulator.

## Sources and attribution

The command interface was checked against Passw/jprx-darwin-vm commit
`ca81f7e8fe7b09d197f99f91189e3f3aed8e09b1`, particularly run.sh, get_files.sh and
README.md. That repository credits jprx; its root-shell results are upstream
claims, not independently reproduced results from this increment.

https://github.com/Passw/jprx-darwin-vm/tree/ca81f7e8fe7b09d197f99f91189e3f3aed8e09b1

Existing repository licenses and notices remain applicable.
