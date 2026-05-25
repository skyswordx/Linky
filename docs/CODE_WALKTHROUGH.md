# Linky Code Walkthrough

这份文档把 Linky 当成代码教材读。
它对应的学习目标是：把“低延迟 Linux 链路如何管理内存和通信”落到 C 代码里。

Linky 不是完整设备驱动，也不假装在 x86 上复现 ARM SoC 的 cache 不一致问题。
它做三件更基础的事：

- 用最少代码展示 buffer pool、fd-backed memory、DMA-BUF heap 的差别；
- 用 descriptor ring 和 eventfd 展示低延迟链路的通信骨架；
- 用 `DMA_BUF_IOCTL_SYNC` 展示 CPU mmap 访问 DMA-BUF 时应该出现的同步边界。

## 文件怎么读

| 文件 | 建议读法 |
|---|---|
| `src/main.c` | 看 CLI 参数怎样切换 `pool / memfd / dmabuf` 三种实验 |
| `src/buffer_pool.c` | 重点看三种 buffer 来源，以及 DMA-BUF CPU sync 调用 |
| `src/ring.c` | 看 producer 和 consumer 如何只传 descriptor |
| `src/runner.c` | 看生产者、消费者、eventfd、时间戳和 latency 统计 |
| `src/time_stats.c` | 看 p50 / p95 / p99 如何从样本里算出来 |

## 三种实验对应什么概念

### 1. `pool`

`pool` 使用 `posix_memalign()` 分配普通用户态内存。

代码位置：

```text
src/buffer_pool.c
  -> linky_pool_alloc_heap()
```

它验证的是“启动阶段预分配、运行阶段复用”。
这块内存没有 fd。
设备不能因为你有一个 `uint8_t*` 就直接访问它。
真实驱动若要 DMA 访问普通用户态页，还要 pin page、建立 DMA 映射，并处理生命周期。

所以 `pool` 适合理解 buffer pool，不适合理解跨设备共享。

### 2. `memfd`

`memfd` 使用：

```text
memfd_create()
  -> ftruncate()
  -> mmap()
```

代码位置：

```text
src/buffer_pool.c
  -> linky_pool_alloc_memfd()
```

这里 fd 指向一个匿名内核文件。
`mmap()` 给当前进程一个 CPU virtual address。

这个实验帮你建立一个直觉：

```text
fd       = 共享对象的句柄
mmap ptr = CPU 读写它时使用的虚拟地址
offset   = descriptor 里描述某个 buffer slice 的位置
```

`memfd` 不是 DMA-BUF。
它可以用于进程间共享内存，但不能直接当作 V4L2/GPU/NPU 可 import 的 dma-buf。

### 3. `dmabuf`

`dmabuf` 使用：

```text
open("/dev/dma_heap/system")
  -> ioctl(DMA_HEAP_IOCTL_ALLOC)
  -> 得到 dma-buf fd
  -> mmap()
```

代码位置：

```text
src/buffer_pool.c
  -> linky_pool_alloc_dmabuf()
```

这一步得到的 fd 对应内核里的 `dma_buf` 对象。
如果系统和设备驱动支持，其他设备驱动可以 import 这个 fd。

这个实验只验证“能否从 DMA-BUF heap 分配、mmap、CPU 读写”。
它没有接 GPU/V4L2/NPU importer。
所以它不能证明某个设备已经能访问这块 buffer。

## cache sync 在代码里怎么看

重点看：

```text
src/buffer_pool.c
  -> linky_pool_begin_cpu_access()
  -> linky_pool_end_cpu_access()
```

生产者写入 frame 前后：

```text
linky_pool_begin_cpu_access(... WRITE)
touch_frame(...)
linky_pool_end_cpu_access(... WRITE)
```

消费者读取 frame 前后：

```text
linky_pool_begin_cpu_access(... READ)
sample_frame(...)
linky_pool_end_cpu_access(... READ)
```

对普通 heap 和 memfd，`pool->fd < 0` 或 fd 不是 dma-buf 时，这些函数没有真实 DMA-BUF sync 意义。
对 dma-buf fd，它们会调用：

```c
ioctl(pool->fd, DMA_BUF_IOCTL_SYNC, &sync)
```

这个 ioctl 的直觉是：

| 调用 | 直觉 |
|---|---|
| START + READ | CPU 准备读共享 buffer；exporter 可以等待设备写完，并让 CPU 避免读旧 cache |
| END + READ | CPU 读完，关闭 CPU 访问窗口 |
| START + WRITE | CPU 准备写共享 buffer |
| END + WRITE | CPU 写完；exporter 可以让 CPU 写入对后续设备可见 |

在 x86 上，很多平台是 cache coherent。
你可能看不到“不 sync 就读旧数据”的现象。
在一些 ARM SoC 或带非一致性 DMA 的设备上，这类边界更关键。

要注意：`DMA_BUF_IOCTL_SYNC` 也不是魔法。
它需要 exporter 和底层驱动正确实现。
如果设备链路还涉及异步硬件任务，仍然需要 fence 或 API 的同步语义来表达任务完成。

## fence 在这个工程里怎么理解

Linky 没有真实 `dma-fence`。
原因很直接：真实 dma-fence 通常由 GPU、DRM、V4L2、媒体驱动或其他异步硬件队列产生。
这个最小 C 工程没有提交真实硬件任务。

Linky 用 `eventfd` 做一个“类 fence 教学模型”。

代码位置：

```text
src/runner.c
  -> notify_event()
  -> wait_for_event()
```

生产者流程：

```text
写 frame
push descriptor 到 ready ring
write(eventfd)
```

消费者流程：

```text
read(eventfd) 等待 ready 信号
pop descriptor
读 frame
```

这个模型只能说明“消费者不应该在生产者声明 ready 之前消费”。
它不能替代真实 dma-fence。

真实 fence 更像这样：

```text
RGA 提交异步任务
  -> 返回 fence fd 或在 dma-buf reservation object 里记录 fence
NPU import 同一个 buffer
  -> 等待 RGA fence signaled
  -> 再开始读 buffer
```

所以要把三件事分开：

| 机制 | 解决什么 |
|---|---|
| ring release/acquire | CPU 线程之间 descriptor 可见性 |
| eventfd | CPU 线程之间的 ready 通知 |
| dma-fence | 设备异步任务完成顺序 |
| DMA_BUF_IOCTL_SYNC | CPU mmap 访问 dma-buf 时的 cache/ownership 边界 |

这也是面试里容易讲清楚的点：

> eventfd 像一个用户态完成通知，fence 是内核里表达硬件任务完成的同步对象。
> cache sync 解决 CPU 和设备看到的数据是否一致，fence 解决上一个设备是否已经写完。

## descriptor ring 为什么只传小结构

Linky 的 descriptor 是：

```c
typedef struct linky_desc {
    uint32_t buffer_id;
    uint32_t sequence;
    uint32_t size;
    uint32_t flags;
    uint64_t timestamp_ns;
    uint64_t offset;
} linky_desc_t;
```

代码位置：

```text
include/linky/linky.h
```

真实视频链路里还会有：

```c
int dma_fd;
uint32_t width;
uint32_t height;
uint32_t format;
uint32_t modifier;
uint32_t stride[4];
uint32_t offset[4];
int acquire_fence_fd;
```

Linky 没有把大图像塞进 ring。
ring 只负责把“哪一个 buffer 准备好了”告诉下游。

这样做有几个好处：

- 队列操作很小，cache footprint 小；
- buffer pool 可以复用；
- 队列满时可以明确统计 dropped；
- 后续接真实设备时，descriptor 可以扩展出 fd、format、stride 和 fence。

## 读代码时最重要的边界

不要把这些概念混在一起：

| 代码位置 | 表示什么 | 不表示什么 |
|---|---|---|
| `posix_memalign()` | 普通 CPU 内存池 | 设备可直接 DMA |
| `memfd_create()` | fd-backed shared memory | DMA-BUF |
| `DMA_HEAP_IOCTL_ALLOC` | 申请 dma-buf fd | 设备已经 import |
| `mmap()` | CPU 虚拟地址映射 | 设备地址 |
| `DMA_BUF_IOCTL_SYNC` | CPU mmap 访问 dma-buf 的同步边界 | 完整硬件 fence 管理 |
| `eventfd` | 线程间 ready 通知 | dma-fence |
| `memory_order_release/acquire` | CPU 线程间可见性 | cache clean/invalidate for DMA |

## 建议实验顺序

先跑：

```bash
./build/linky pool --frames 20000 --frame-size 1M --buffers 8
./build/linky memfd --frames 20000 --frame-size 1M --buffers 8 --mlockall
```

再比较 eventfd 和 busy-yield：

```bash
./build/linky pool --frames 20000 --frame-size 1M
./build/linky pool --frames 20000 --frame-size 1M --no-eventfd
```

如果有 DMA-BUF heap：

```bash
ls /dev/dma_heap
./build/linky dmabuf --heap /dev/dma_heap/system --frames 5000 --frame-size 1M
```

再试线程绑定：

```bash
./build/linky memfd --frames 20000 --producer-cpu 2 --consumer-cpu 3 --mlockall
```

记录 p95、p99、max。
不要只看 avg。
