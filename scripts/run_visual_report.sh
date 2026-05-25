#!/usr/bin/env bash
set -euo pipefail

exe="${1:-./build/linky}"
out_dir="${2:-reports}"

python3 scripts/visualize_learning_labs.py \
  --exe "$exe" \
  --out-dir "$out_dir" \
  --sudo-dmabuf
