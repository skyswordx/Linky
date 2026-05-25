#!/usr/bin/env bash
set -euo pipefail

exe="${1:-./build/linky}"
out="${2:-linky_learning_labs.csv}"

tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT

"$exe" pool --frames 10 --frame-size 4K --buffers 4 --csv >/dev/null

"$exe" pool --frames 1 --frame-size 4K --buffers 4 --csv >"$tmp" || true
head -n 0 "$tmp" >/dev/null

cat >"$out" <<'CSV'
mode,frame_size,buffers,iterations,eventfd,mlockall,producer_cpu,consumer_cpu,rt_prio,samples,dropped,elapsed_ms,avg_us,p50_us,p95_us,p99_us,max_us,lab,note
CSV

run_one() {
  local lab="$1"
  local note="$2"
  shift 2
  local row
  row="$("$exe" "$@" --csv)"
  printf '%s,%s,%s\n' "$row" "$lab" "$note" >>"$out"
}

echo "[1/5] frame-size sweep"
for size in 4K 64K 1M 4M; do
  run_one frame_size "larger_frame_touches_more_cache_lines" pool --frames 10000 --frame-size "$size" --buffers 8 --ring 64
done

echo "[2/5] backend comparison"
for mode in pool memfd; do
  run_one backend "same_descriptor_pipeline_different_backing_store" "$mode" --frames 20000 --frame-size 1M --buffers 8 --ring 64
done
if [[ -e /dev/dma_heap/system ]]; then
  run_one backend "dmabuf_requires_root_or_device_permission" dmabuf --heap /dev/dma_heap/system --frames 5000 --frame-size 1M --buffers 8 --ring 64
fi

echo "[3/5] eventfd vs busy-yield"
run_one notify "eventfd_sleep_wakeup_path" pool --frames 20000 --frame-size 1M --buffers 8 --ring 64
run_one notify "busy_yield_polling_path" pool --frames 20000 --frame-size 1M --buffers 8 --ring 64 --no-eventfd

echo "[4/5] buffer-count sweep"
for buffers in 2 4 8 16; do
  run_one buffers "small_pool_has_less_slack_large_pool_has_more_inflight_memory" pool --frames 20000 --frame-size 1M --buffers "$buffers" --ring 64
done

echo "[5/5] mlockall and affinity"
run_one scheduling "baseline_no_lock_no_affinity" memfd --frames 20000 --frame-size 1M --buffers 8 --ring 64
run_one scheduling "mlockall_reduces_page_fault_jitter_when_allowed" memfd --frames 20000 --frame-size 1M --buffers 8 --ring 64 --mlockall
if [[ "$(nproc)" -ge 4 ]]; then
  run_one scheduling "producer_consumer_on_different_cpus" memfd --frames 20000 --frame-size 1M --buffers 8 --ring 64 --mlockall --producer-cpu 2 --consumer-cpu 3
fi

echo "wrote $out"
echo "Try: column -s, -t $out | less -S"
