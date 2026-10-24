#!/bin/bash
# Native Linux host test using Mesa software OpenGL; does not enable a physical GPU.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
[[ $(uname -s) == Linux ]] || { echo 'This test adapter requires a Linux host.' >&2; exit 2; }
BUILD="$ROOT/build/virgl"
mkdir -p "$BUILD"
export HACKIOS_QEMU_REAL
HACKIOS_QEMU_REAL=$(command -v qemu-system-aarch64)
[[ $(realpath "$HACKIOS_QEMU_REAL") != "$ROOT/experimental/virgl/bin/qemu-system-aarch64" ]] || {
    echo 'Recursive QEMU wrapper path; start from the normal host PATH.' >&2; exit 2;
}
export HACKIOS_SURFACELESS_ADAPTER="$BUILD/qemu-surfaceless.so"
"${HOST_CC:-gcc}" -shared -fPIC -O2 -Wall -Wextra -Werror \
    "$ROOT/experimental/virgl/qemu-surfaceless.c" -o "$HACKIOS_SURFACELESS_ADAPTER.tmp" -ldl
mv "$HACKIOS_SURFACELESS_ADAPTER.tmp" "$HACKIOS_SURFACELESS_ADAPTER"
# No system library or QEMU binary is changed. Only the wrapper adds LD_PRELOAD.
export PATH="$ROOT/experimental/virgl/bin:$PATH"
export LIBGL_ALWAYS_SOFTWARE=1 MESA_SHADER_CACHE_DISABLE=true EGL_PLATFORM=surfaceless
if [[ -z "${XDG_RUNTIME_DIR:-}" ]]; then
    export XDG_RUNTIME_DIR="$BUILD/runtime"
    mkdir -p "$XDG_RUNTIME_DIR"
    chmod 700 "$XDG_RUNTIME_DIR"
fi
export VIRGL=1 VIRGL_SELFTEST=1
exec bash "$ROOT/scripts/run-qemu.sh" "$@"
