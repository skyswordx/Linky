# Linky

Linky is a small Linux C lab for validating low-latency buffer ownership, fd-backed memory, and descriptor-ring communication.
It is meant to be built on Linux x86 first.
It does not require an ARM board.

The project contains three experiments:

| Mode | What it validates | Hardware requirement |
|---|---|---|
| `pool` | preallocated heap buffer pool, SPSC descriptor rings, eventfd notification, p50/p95/p99 latency | any Linux host |
| `memfd` | fd-backed shared memory with `memfd_create()` + `mmap()`, descriptor passing by offset/index | Linux kernel with `memfd_create` |
| `dmabuf` | DMA-BUF heap allocation through `/dev/dma_heap/*`, mmap of exported fd, same descriptor-ring pipeline | Linux kernel with DMA-BUF heaps enabled |

The goal is not to pretend that x86 reproduces Rockchip RGA/RKNN behavior.
The goal is to verify the design habits behind a low-latency Linux data path:

- allocate buffers before the hot path;
- pass descriptors instead of copying large frames between stages;
- distinguish fd, CPU virtual address, and device-visible DMA address;
- keep queue depth bounded;
- measure tail latency, not only average latency;
- make platform-specific DMA-BUF availability explicit.

## Build

```bash
git clone https://github.com/skyswordx/Linky.git
cd Linky
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Run

```bash
./build/linky selftest

./build/linky pool \
  --frames 20000 \
  --frame-size 1M \
  --buffers 8 \
  --ring 64

./build/linky memfd \
  --frames 20000 \
  --frame-size 1M \
  --buffers 8 \
  --ring 64 \
  --mlockall
```

If your kernel exposes DMA-BUF heaps:

```bash
ls /dev/dma_heap

./build/linky dmabuf \
  --heap /dev/dma_heap/system \
  --frames 5000 \
  --frame-size 1M \
  --buffers 8 \
  --ring 64
```

You can also run the bundled script:

```bash
bash scripts/run_experiments.sh ./build/linky
```

## Useful options

| Option | Meaning |
|---|---|
| `--frames N` | number of produced descriptors |
| `--frame-size SIZE` | bytes per buffer, supports `K`, `M`, `G` suffixes |
| `--buffers N` | number of buffers in the pool |
| `--ring N` | descriptor ring capacity |
| `--no-eventfd` | replace eventfd wakeup with busy-yield polling |
| `--mlockall` | request `mlockall(MCL_CURRENT | MCL_FUTURE)` |
| `--producer-cpu N` | bind producer thread to CPU `N` |
| `--consumer-cpu N` | bind consumer thread to CPU `N` |
| `--rt-prio N` | request `SCHED_FIFO` priority `N`, often needs privilege |
| `--heap PATH` | DMA-BUF heap device path |

## What to compare

Run the same parameters across `pool`, `memfd`, and `dmabuf`.
Compare:

- `samples`
- `dropped`
- `elapsed_ms`
- `avg`
- `p50`
- `p95`
- `p99`
- `max`

The first result to watch is p99.
Average latency can look fine while tail latency is already too high for real-time vision.

## How this maps to a real vision/AI pipeline

In a real VPU/RGA/NPU pipeline, the descriptor would carry more metadata:

```c
struct VideoBufferDesc {
    int dma_fd;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t modifier;
    uint32_t stride[4];
    uint32_t offset[4];
    int acquire_fence_fd;
};
```

Linky keeps the descriptor smaller because it is a minimal lab.
The important part is the direction:
hot-path stages pass a buffer id, fd-backed offset, timestamp, and sequence number.
They do not copy megabytes of image data through queues.

## Limits

x86 Linux is good for validating memory ownership, fd-backed mapping, queueing, wakeup, scheduling, and latency measurement.
It may not expose the same cache-coherency problems as non-coherent ARM SoCs.
It also does not validate Rockchip RGA, RKNN, or MPP behavior.

The DMA-BUF experiment checks whether a heap exists and whether a fd can be allocated and mmaped.
It does not prove that a GPU, V4L2 device, or NPU can import that fd.
Device import must be tested with the real driver stack.

## Read as a code textbook

Read [docs/CODE_WALKTHROUGH.md](docs/CODE_WALKTHROUGH.md) after the first build.
It explains the important code paths:

- where fd-backed memory is created;
- where `mmap()` turns an fd into a CPU virtual address;
- where `DMA_BUF_IOCTL_SYNC` marks CPU read/write access to a dma-buf;
- why `eventfd` in this lab is only a fence-like teaching model;
- why `memory_order_release/acquire` is CPU thread ordering, not DMA cache maintenance.

For the specific cache-sync and fence distinction, read
[docs/CACHE_SYNC_AND_FENCE.md](docs/CACHE_SYNC_AND_FENCE.md).

For a KyLink-oriented learning path, read
[docs/KYLINK_LEARNING_LABS.md](docs/KYLINK_LEARNING_LABS.md).
It explains how to turn the raw `avg / p95 / p99 / max` numbers into practical intuition for a video inference pipeline.

To generate a CSV table for the learning labs:

```bash
bash scripts/run_learning_labs.sh ./build/linky linky_learning_labs.csv
column -s, -t linky_learning_labs.csv | less -S
```

To generate a visual report:

```bash
bash scripts/run_visual_report.sh ./build/linky reports
xdg-open reports/linky_learning_labs.html
```

The visual report runs the same first-principles sequence, writes the raw CSV,
and creates a standalone HTML file for comparing p99 and max across experiments.
