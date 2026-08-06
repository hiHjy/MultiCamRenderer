# 开发日志

## 2026-08-06

### 项目当前进度

项目当前已经从摄像头采集 demo 进入到初步视频框架阶段，核心链路已经跑通：

```text
V4L2CameraSource
  -> CamManager 统一 poll
  -> FrameHub 分发
  -> Consumer::onFrame()
```

目前已经完成的基础模块：

- `V4L2CameraSource`：负责打开摄像头、协商格式、申请 DMA buffer、导入 V4L2、`DQBUF/QBUF`。
- `CamManager`：负责统一管理多路摄像头，并用同一个 `poll()` 调度多个摄像头 fd。
- `FrameHub`：负责把某一路流的 `VideoFrame` 分发给订阅的消费者。
- `Consumer`：消费者接口，目前约定 `onFrame()` 中拿到的是短生命周期帧视图，不能长期持有摄像头原始 buffer。
- `DmaAllocator` / `DmaMemory`：基于 `/dev/dma_heap` 的 DMA 内存申请和 RAII 释放。
- `DmaBufferPool`：消费者侧使用的 DMA buffer 池，只负责 DMA 内存的借出和归还，不绑定图像格式、宽高、stride。
- `RgaEngine`：RGA 基础封装，目前支持同格式 `copy()` 和 `resize()`。
- `RgaCopyConsumer`：当前是测试消费者，用来验证 V4L2 DMA-BUF 到消费者私有 DMA-BUF 的 RGA copy 链路。

当前已经验证过：

- RV1126 32 位构建链路可用，`build.sh` 可以编译 `camera_capture_demo`、`cam_manager_demo`、`dma_allocator_demo`、`test`、`rga_test`。
- aarch64 测试构建脚本 `build_test_aarch64.sh` 可用。
- 摄像头 `/dev/video32` 可以跑 1280x720 NV12。
- RGA 可以把摄像头 DMA-BUF 拷贝到消费者自己的 DMA-BUF。
- 1280x720 NV12 的 RGA copy 大约稳定在 `1.7ms` 左右。

### 今天完成的事

1. 明确了 `DmaBufferPool` 的职责边界。

   最终决定：它不是图像帧池，而是纯 DMA buffer 池。它只负责申请一批 DMA 内存、借出、归还。它不在初始化时绑定 `width/height/format/stride`。

2. 明确了 `VideoFrame` 的语义。

   `VideoFrame` 是一块底层资源在当前时刻的图像解释，包含：

   - `dmaFd / va / capacity`：底层 DMA 资源信息。
   - `width / height / format / stride / heightStride`：当前图像 layout。

   重要原则：

   ```text
   谁往 buffer 里生产图像，谁负责填写真实 layout。
   ```

3. 给 `VideoFrame` 增加了 `heightStride`。

   目前约定：

   - `stride == 0`：使用方默认按 `width` 处理。
   - `heightStride == 0`：使用方默认按 `height` 处理。
   - V4L2 当前不可靠提供纵向 stride，所以保持 `heightStride = 0`。
   - MPP 解码输出后续如果有 `hor_stride / ver_stride`，再由 MPP 模块填写真实值。

4. 将 `DmaFramePool` 改名为 `DmaBufferPool`。

   改名原因：当前池子只管理 DMA buffer，不管理成品帧顺序，也不表达固定图像格式。消费者自己的 ready queue 后续由具体消费者自己实现。

5. 调整了 `RgaEngine` 的 stride 处理。

   RGA 调用前会计算有效 stride：

   ```text
   effectiveStride       = frame.stride > 0 ? frame.stride : frame.width
   effectiveHeightStride = frame.heightStride > 0 ? frame.heightStride : frame.height
   ```

   并且在调用 RGA 前按最终 layout 检查 `capacity`，避免硬件越界读写。

6. 增加并验证了 `rga_test`。

   `build.sh` 新增 `build/rga_test` 目标，用于测试：

   ```text
   摄像头 DMA-BUF -> RGA copy -> 消费者私有 DMA-BUF
   ```

   板端测试输出连续 `rga success`，1280x720 NV12 copy 耗时约 `1.7ms`。

7. 给关键代码补充中文注释。

   重点注释了：

   - `VideoFrame` 中资源字段和 layout 字段的区别。
   - `DmaBufferPool` 只管内存，不管图像 layout。
   - `RgaEngine` 中 stride 默认值和 capacity 检查。
   - `V4L2CameraSource` 不乱填 `heightStride`。
   - `RgaCopyConsumer` 当前只是同步测试版，真正消费者后续应使用自己的 ready queue 和 worker 线程。

### 重要设计结论

当前架构先不做完整 GStreamer 式 pipeline，继续保持：

```text
CamManager
  -> FrameHub
  -> Consumer
```

慢消费者自己负责：

- RGA copy 到自己的 `DmaBufferPool`。
- 把 copy 完成的 `VideoFrame*` 放入自己的 ready queue。
- worker 线程处理完成后再 `releaseFrame()`。
- 队列满时由消费者自己决定丢旧帧还是丢新帧。

这样 `FrameHub` 和 `CamManager` 保持简单，摄像头原始 V4L2 buffer 可以尽快 `QBUF` 归还。
