#!/usr/bin/env bash
set -euo pipefail

exe="${1:-./build/linky}"

"$exe" selftest
"$exe" pool --frames 20000 --frame-size 1M --buffers 8 --ring 64
"$exe" memfd --frames 20000 --frame-size 1M --buffers 8 --ring 64 --mlockall

if [[ -e /dev/dma_heap/system ]]; then
  "$exe" dmabuf --heap /dev/dma_heap/system --frames 5000 --frame-size 1M --buffers 8 --ring 64
else
  echo "skip dmabuf: /dev/dma_heap/system not found"
fi
