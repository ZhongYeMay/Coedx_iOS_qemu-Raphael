#!/bin/bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$ROOT/build/host"
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}
for name in touch i2c a640 virtio_gpu ufs_policy; do
    "${CC:-gcc}" -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
      -I "$ROOT/overlay/xnu/files/bsd/dev" -I "$ROOT/overlay/xnuloader" \
      "$ROOT/tests/${name}_test.c" -o "$ROOT/build/host/${name}_test"
    "$ROOT/build/host/${name}_test"
done
