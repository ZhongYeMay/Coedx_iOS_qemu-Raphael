#!/bin/bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORKSPACE=${1:-$ROOT/work/hackios}
SECONDS_TO_RUN=${2:-900}
[[ "$SECONDS_TO_RUN" =~ ^[0-9]+$ ]] || { echo 'seconds must be an integer' >&2; exit 2; }
WORKSPACE=$(realpath "$WORKSPACE")
command -v qemu-system-aarch64 >/dev/null
export QEMU_EFI=${QEMU_EFI:-/usr/share/AAVMF/AAVMF_CODE.fd}
export QEMU_VARS=${QEMU_VARS:-/usr/share/AAVMF/AAVMF_VARS.fd}
export QEMU_SMP=${QEMU_SMP:-2}
export QEMU_RAM_GB=${QEMU_RAM_GB:-7}
export CMDLINE="cpus=$QEMU_SMP virtio_gpu=1 ${CMDLINE:-}"
args=(-nic none -device virtio-gpu-device,id=vgpu,xres=1125,yres=2048)
if [[ -n "${QEMU_DATA_DIR:-}" ]]; then args+=(-L "$QEMU_DATA_DIR"); fi
[[ -f "$QEMU_EFI" && -f "$QEMU_VARS" ]] || { echo 'Set QEMU_EFI/QEMU_VARS to your AArch64 UEFI firmware files.' >&2; exit 2; }
cd "$WORKSPACE"
exec bash qemu/run-ios.sh "$SECONDS_TO_RUN" "${args[@]}"
