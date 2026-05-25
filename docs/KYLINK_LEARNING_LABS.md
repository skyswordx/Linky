# KyLink-Oriented Learning Labs

你现在看到的几行输出还很抽象：

```text
avg=70us p50=68us p95=86us p99=114us max=648us
```

第一性原理的训练方法不是盯着单次数字看，而是一次只改变一个变量。
你要观察的是变量改变后，p50、p99、max 和 dropped 怎样变化。

这份文档把 Linky 的实验映射回 KyLink。

## 先解释你刚跑出的结果

你的结果大致是：

| mode | avg | p99 | 直觉 |
|---|---:|---:|---|
| pool | 70 us | 114 us | 普通用户态 buffer pool，CPU 写、CPU 读、descriptor ring 通知 |
| memfd | 75 us | 124 us | fd-backed shared memory，仍然是 CPU 访问 RAM，所以不会天然大幅变快 |
| dmabuf | 76 us | 121 us | DMA-BUF heap + mmap + sync ioctl；没有真实设备 import，所以延迟仍接近 CPU 访问路径 |

这组结果说明三件事。

第一，fd-backed memory 不是性能魔法。
`memfd` 和 `dmabuf` 的意义在于表达共享内存对象和 fd 生命周期。
如果还是 CPU 通过 mmap 写读，延迟主要由 CPU cache、调度和内存访问决定。

第二，DMA-BUF 只有在“跨设备 import 同一块 buffer”时才真正体现价值。
Linky 的 `dmabuf` 模式只验证 allocator、fd、mmap 和 CPU access sync。
它没有接 V4L2、GPU、RGA 或 NPU，所以不能期待它比普通 pool 快。

第三，`max` 比 `avg` 更能提醒你系统抖动。
视觉链路里偶发 1ms 抖动可能比平均值多 5us 更重要。
KyLink 面试里也应该强调 p95/p99，而不是只说 FPS。

## 跑学习实验

在 Linux 主机上执行：

```bash
bash scripts/run_learning_labs.sh ./build/linky linky_learning_labs.csv
```

查看：

```bash
column -s, -t linky_learning_labs.csv | less -S
```

如果你想直接生成纵览报告，执行：

```bash
bash scripts/run_visual_report.sh ./build/linky reports
xdg-open reports/linky_learning_labs.html
```

这个脚本会自动跑 frame size、backend、eventfd、buffer count、scheduling 几组实验。
原始数据保存在：

```text
reports/linky_learning_labs.csv
```

可视化报告保存在：

```text
reports/linky_learning_labs.html
```

HTML 里每个实验都会画出 p99 和 max。
先看 p99，再看 max。
平均值只作为参考。

## 实验 1：frame size sweep

命令里会跑：

```text
4K / 64K / 1M / 4M
```

你要观察：

- frame_size 增大后，elapsed 和 p99 是否上升；
- max 是否出现更大抖动；
- 这说明“图像帧越大，CPU 碰大图像越贵”。

映射回 KyLink：

KyLink 里如果 CPU 做 NV12 -> RGB、resize、letterbox，会触碰大块图像内存。
RGA 的价值在于把这些大块图像操作交给硬件。
但 RGA 输出如果又回到 CPU buffer，再进入 RKNN，仍然有 CPU / runtime 输入路径成本。

## 实验 2：backend comparison

比较：

```text
pool vs memfd vs dmabuf
```

你要建立的直觉：

- pool 最简单，但没有 fd 语义；
- memfd 有 fd + mmap，但不是 DMA-BUF；
- dmabuf 有跨设备共享潜力，但只有接真实 importer 才能体现。

映射回 KyLink：

GStreamer 里的 DMA-BUF fd 不能当成 `uint8_t*`。
你还要拿到 format、stride、offset。
RGA import 这个 fd 才是“设备侧使用共享 buffer”的开始。

## 实验 3：eventfd vs busy-yield

比较：

```text
eventfd on
eventfd off (--no-eventfd)
```

你要观察：

- eventfd 让消费者睡眠等待 ready；
- busy-yield 可能降低一点 wakeup 开销，但会占 CPU；
- p99/max 可能受调度影响。

映射回 KyLink：

eventfd 只是用户态 ready 通知。
真实设备链路里，RGA/NPU 的异步完成要看 API 返回、阻塞调用、回调、poll fd 或 fence fd。
不能把 eventfd 说成 dma-fence。

## 实验 4：buffer-count sweep

比较：

```text
buffers = 2 / 4 / 8 / 16
```

你要观察：

- buffer 太少，下游慢一点就容易卡住上游；
- buffer 太多，会增加 in-flight 数据和延迟积累风险；
- 实时系统常常宁愿丢旧帧，也不愿无限排队。

映射回 KyLink：

摄像头 / 解码 / RGA / NPU 之间如果每层都排很多帧，总 FPS 可能还行，但端到端延迟会变大。
面试里可以说你关注的是分阶段耗时和队列等待，而不是只看整体 FPS。

## 实验 5：mlockall 和 CPU affinity

比较：

```text
baseline
--mlockall
--mlockall --producer-cpu 2 --consumer-cpu 3
```

你要观察：

- `mlockall` 是否降低 max；
- CPU 亲和性是否让 p99 更稳定；
- 结果可能因机器负载而变化。

映射回 KyLink：

RK3566 这类边缘设备上，预处理线程、推理线程、UI 线程抢 CPU 会影响尾延迟。
如果继续做系统级优化，可以考虑线程亲和性、优先级、IRQ 分配和性能模式。

## cache sync 应该怎么建立直觉

Linky 里 `dmabuf` 模式会在 CPU 访问 mmap 后的 dma-buf 前后调用：

```text
BEGIN WRITE
CPU writes frame
END WRITE

BEGIN READ
CPU reads frame
END READ
```

对应代码：

```text
src/runner.c
src/buffer_pool.c
```

你要理解的是“访问窗口”：

- CPU 准备写共享 buffer，要声明开始写；
- CPU 写完，要声明结束写，让后续设备有机会看到数据；
- CPU 准备读设备写过的 buffer，要声明开始读，让 CPU 避免读旧 cache；
- CPU 读完，要声明结束读。

x86 通常 cache coherent，所以你不一定能观察到“不 sync 就错”的现象。
但这个调用位置就是以后读 ARM / Rockchip / V4L2 / DRM 代码时要找的边界。

## fence 应该怎么建立直觉

Linky 的 eventfd 只模拟 ready 通知：

```text
producer 写完
producer 通知
consumer 再读
```

真实 fence 是设备任务完成：

```text
RGA 提交任务
RGA 写 buffer
RGA fence signaled
NPU 等 fence
NPU 读 buffer
```

映射回 KyLink：

当前 KyLink 主要依赖 RGA/RKNN API 的同步调用语义。
你不要说自己手写了 fence 管理。
更稳的说法是：

> 我理解 fence 解决设备异步任务完成顺序。KyLink 当前没有直接操作 fence fd，而是通过 GStreamer、RGA、RKNN API 的同步语义组织处理顺序。

## 最终要形成的直觉

把 Linky 和 KyLink 连起来看：

| Linky 学到的东西 | KyLink 里的对应问题 |
|---|---|
| fd 只是共享对象句柄 | GStreamer DMA-BUF fd 不能单独代表图像 |
| mmap 得到的是 CPU 虚拟地址 | 设备访问还需要 import / DMA 映射 |
| descriptor ring 只传元数据 | 视频链路应该传 fd、format、stride、offset |
| eventfd 是 ready 通知 | 真实设备同步还要看 fence / API 完成语义 |
| DMA_BUF_IOCTL_SYNC 是 CPU access 边界 | CPU mmap 访问共享 buffer 时要考虑 cache |
| p99/max 反映抖动 | KyLink 性能快照要看分阶段尾延迟 |

你不需要一次把所有 Linux 底软都学完。
先把这几个边界讲清楚，面试就很难被“DMA-BUF fd 是不是地址”“零拷贝是不是一定快”“cache 和 fence 是不是一回事”这类问题打穿。
