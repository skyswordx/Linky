# Cache Sync and Fence Notes

这份文档只解释两个容易混在一起的概念：

- cache sync：CPU 和设备看到的数据是否一致；
- fence：上一个异步任务是否已经完成。

Linky 不能在 x86 主机上制造完整的非一致性 DMA 场景。
它的价值在于把这两个边界放到代码里，让你以后读 V4L2、DRM、RGA、NPU API 时知道自己在找什么。

## 场景 1：CPU 写，设备读

典型例子：CPU 准备一块输入张量，然后 NPU 读取。

```text
CPU writes buffer through mmap
  |
  | dirty cache lines may still live in CPU cache
  v
CPU ends write access
  |
  | cache clean / flush may be needed
  v
Device reads buffer through DMA
```

如果缺少同步，设备可能读到旧数据。

Linky 中对应位置：

```text
src/runner.c
  -> producer_main()
       linky_pool_begin_cpu_access(... WRITE)
       touch_frame(...)
       linky_pool_end_cpu_access(... WRITE)
```

`LINKY_CPU_ACCESS_WRITE` 对应 `DMA_BUF_SYNC_WRITE`。
`END + WRITE` 是关键边界，因为 CPU 写完后，后续设备才应该看到完整数据。

## 场景 2：设备写，CPU 读

典型例子：摄像头、VPU 或 RGA 写入一帧图像，然后 CPU 读取。

```text
Device writes buffer through DMA
  |
  | CPU cache may still contain old lines
  v
CPU begins read access
  |
  | cache invalidate may be needed
  v
CPU reads fresh data
```

如果缺少同步，CPU 可能读到上一帧或旧内容。

Linky 中对应位置：

```text
src/runner.c
  -> consumer_main()
       linky_pool_begin_cpu_access(... READ)
       sample_frame(...)
       linky_pool_end_cpu_access(... READ)
```

`LINKY_CPU_ACCESS_READ` 对应 `DMA_BUF_SYNC_READ`。
`START + READ` 是关键边界，因为 CPU 读之前要确认看到的是最新数据。

## 场景 3：设备 A 写，设备 B 读

典型例子：RGA 写 RGB buffer，NPU 读取 RGB buffer。

这里 CPU 不一定碰图像数据。
cache sync 仍然可能存在，但更明显的问题是设备任务顺序。

```text
RGA starts writing output buffer
  |
  | asynchronous hardware work
  v
RGA signals fence
  |
  | NPU waits for fence
  v
NPU reads output buffer
```

如果没有 fence 或等价同步，NPU 可能在 RGA 还没写完时就读。
这会得到半帧或未定义内容。

Linky 没有真实 RGA/NPU，所以没有真实 `dma-fence`。
它用 eventfd 做教学模型：

```text
producer writes buffer
producer pushes descriptor
producer writes eventfd

consumer reads eventfd
consumer pops descriptor
consumer reads buffer
```

对应代码：

```text
src/runner.c
  -> notify_event()
  -> wait_for_event()
```

eventfd 只能表达“用户态生产者通知用户态消费者”。
真实 fence 表达的是“内核里的异步硬件任务完成”。

## 四种同步不要混淆

| 机制 | 发生在哪里 | 解决什么 |
|---|---|---|
| `memory_order_release/acquire` | CPU 线程之间 | descriptor 字段对另一个 CPU 线程可见 |
| `eventfd` | 用户态 / 内核事件 fd | 一个线程通知另一个线程有新 descriptor |
| `DMA_BUF_IOCTL_SYNC` | CPU mmap 访问 dma-buf 前后 | CPU cache 与共享 buffer 的访问边界 |
| `dma-fence` | 内核设备同步框架 | 异步硬件任务是否完成 |

这四个机制经常一起出现，但不能互相替代。

## 面试表达

可以这样说：

> 我把同步分成两类。
> 第一类是 CPU 访问共享 buffer 的 cache/ownership 同步，比如 mmap DMA-BUF 前后的 `DMA_BUF_IOCTL_SYNC`。
> 第二类是设备任务顺序同步，比如 RGA 写完后 NPU 才能读，这类要靠 fence 或平台 API 的完成语义。
> eventfd 和 ring 只是我在用户态实验里模拟 ready 通知的方法，不能把它说成真实 dma-fence。

## 在真实项目里怎么落地

如果以后接 V4L2 / DRM / RGA / NPU，重点看 API 有没有这些信息：

- buffer fd 从哪里来，谁是 exporter；
- importer 是否显式 attach / import；
- CPU 是否 mmap 访问过 buffer；
- CPU 访问前后是否需要 sync ioctl；
- 异步设备任务是否返回 fence fd；
- 下游 API 是否等待 fence；
- format、stride、offset、modifier 是否随 fd 一起传递。

只要这几个问题能答清楚，就不会把 DMA-BUF 简化成“传一个 fd 就零拷贝”。
