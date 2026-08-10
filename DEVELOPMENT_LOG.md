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

## 2026-08-09

### 项目当前进度

补充：本日继续处理 Qt aarch64 交叉编译环境。将 `qt6-aarch64.tar..xz` 安装到主机 `/opt/Qt/6.10.3-rk3568-aarch64`，并创建 `/opt/6.10.3-rk3568-aarch64` 路径用于和板端运行脚本保持一致。

`qt-demo/toolchain.cmake` 已从旧的 `/home/alientek/...` 路径调整为当前 Ubuntu 上真实存在的：

```text
toolchain: /home/hjy/rk3568_kernel_pack/toolchain/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu
sysroot:   /home/hjy/rk3568_kernel_pack/sysroot
host Qt:   /opt/Qt/6.10.3
target Qt: /opt/6.10.3-rk3568-aarch64
```

当前 `qt-demo/build.sh` 可以通过 CMake 正常交叉编译 `appqt-demo`，生成 aarch64 可执行文件。板端通过：

```bash
cd /root/nfs/project/qt-demo
./run.sh
```

已验证 Qt EGLFS 可以启动，输出窗口尺寸 `1920x1080`，`DisplayConsumer FPS` 约 30。

注意：运行日志中出现过 `No QSGTexture provided from updateSampledImage(). This is wrong.`，后续接着检查 Qt Quick texture 提供时机或空帧路径。

这一版从“摄像头采集 + RGA copy 测试”推进到了可用的 Qt Quick DMA-BUF 显示 demo，并且开始把底层帧生命周期模型收紧。当前主链路已经变成：

```text
V4L2CameraSource
  -> CamManager DQBUF
  -> FramePacket + FrameLease
  -> FrameHub weak_ptr 分发
  -> Consumer/Sink 快速处理
  -> lease 释放后投递 return queue
  -> CamManager 统一 QBUF
```

这里的核心变化是：`VideoFrame` 仍然只描述图像和 DMA 资源，真正保护 V4L2 buffer 生命周期的是外层 `FramePacket::lease`。

### 今天完成的事

1. 新增 Qt Quick DMA-BUF 显示 demo。

   新增 `qt-demo`，实现了从摄像头 YUYV 帧到 Qt Quick 纹理显示的链路：

   ```text
   V4L2 YUYV DMA-BUF
     -> DisplayConsumer 使用 RGA 转 RGBA
     -> DmaBufferPool 保存稳定 RGBA 帧
     -> Qt Quick 通过 EGLImage 导入 DMA-BUF
     -> QSGTexture 缓存后显示
   ```

   当前板端测试约 `30fps`，纹理按 DMA fd 缓存，不再每帧重建 EGL/GL 纹理。

2. 新增 `DisplayConsumer`。

   `DisplayConsumer` 作为显示 sink，目前策略是：

   - 收到 V4L2 `FramePacket` 后快速 RGA 到自己的 RGBA `DmaBufferPool`。
   - 转换成功后把稳定的 `DisplayFrame` 通过 Qt signal 发给 QML item。
   - Qt 渲染侧显示完成后，再释放对应 RGBA pool buffer。

   这一版没有强制给 `DisplayConsumer` 增加额外 worker 队列，因为它的工作就是一次快速硬件转换，符合“sink 自己决定处理策略”的原则。

3. 扩展 `RgaEngine` 为统一操作封装。

   `RgaEngine` 从简单 `copy/resize` 扩展为统一入口：

   ```cpp
   bool rga(const VideoFrame& src, VideoFrame& dst, const RgaOperation& op);
   ```

   当前支持：

   - copy
   - resize
   - color convert
   - crop
   - rotate 0/90/180/270
   - mirror horizontal/vertical/both

   同时补充了输出几何计算逻辑：例如旋转 90/270 时自动交换宽高，crop 时按裁剪区域决定输出尺寸。

4. 明确了 RGA stride 和 buffer size 的边界。

   当前约定：

   - `DmaBufferPool` 仍然只是通用 DMA 内存池，不绑定 RGA。
   - RGA 运行时 layout 默认按 16 字节 pitch 对齐处理。
   - `RgaEngine::bufferSizeFor()` 默认按 64 字节 pitch 对齐估算容量，用于给 DMA 池预留更保守的空间。

   这里要注意：64 字节是池子容量预留策略，不是强行把所有 RGA 运行时 stride 都改成 64 字节。

5. 增强 `DmaAllocator`。

   DMA 分配现在优先尝试 `/dev/dma_heap`，失败后 fallback 到 DRM dumb buffer：

   ```text
   dma_heap
     -> /dev/dri/card*
     -> /dev/dri/renderD*
     -> 全部失败后返回错误
   ```

   DRM fallback 使用 ioctl 直接实现，不额外依赖 libdrm。`DmaMemory` 也补齐了 DRM dumb handle 的 RAII 清理，避免 fd/handle 泄漏。

6. 新增 RGA 操作测试 demo。

   新增 `demo/rga`，从 `img/1.png` 读取图片，在板端通过 RGA 生成各类操作结果：

   - copy
   - resize
   - crop
   - rotate
   - mirror
   - RGBA -> YUYV -> RGBA 转换链路
   - crop + rotate / crop + mirror 组合操作

   测试输出 png 本次作为样例结果提交到仓库，方便后续对照 RGA 输出是否符合预期。

7. 新增 `FrameLease` 生命周期演示 demo。

   新增 `demo/frame_lease`，用纯 C++ 模拟：

   ```text
   FakeCamera DQBUF
     -> 创建 FrameLease
     -> 分发给 display/record sink
     -> sink 只保留最新帧
     -> 最后一个 shared_ptr 释放
     -> 自动归还 buffer
   ```

   这个 demo 用来理解 `shared_ptr + RAII release callback` 的语义，不依赖真实摄像头。

8. 将 `Consumer` 接口升级为 `FramePacket`。

   原接口：

   ```cpp
   virtual void onFrame(const VideoFrame& frame) = 0;
   ```

   第一版新接口：

   ```cpp
   virtual void onFrame(const FramePacket& packet) = 0;
   ```

   `FramePacket` 包含：

   ```cpp
   struct FramePacket {
       VideoFrame frame;
       std::shared_ptr<FrameLease> lease;
   };
   ```

   这样 sink 如果需要异步持有 V4L2 帧，可以复制 `lease` 延长生命周期；如果只是快速 RGA 到自己的池子，则处理完直接返回即可。

   后续复盘时发现这里还不够准确：如果希望 sink 真正拥有自己的 lease 引用，接口应该按值传递 `FramePacket`，否则 `const FramePacket&` 只是借看。

   因此本次继续调整为：

   ```cpp
   virtual void onFrame(FramePacket packet) = 0;
   ```

   这样每个 sink 都会拿到自己那份 `shared_ptr<FrameLease>` 引用。同步 sink 可以函数结束自动释放；异步 sink 可以 `std::move(packet)` 到自己的队列或 latest slot 中，处理完成后再释放 lease。

9. `FrameHub` 改为弱引用消费者。

   `FrameHub` 不再拥有消费者对象，而是保存 `std::weak_ptr<Consumer>`：

   ```text
   真正消费者模块持有 shared_ptr
   FrameHub 只保存 weak_ptr
   publish 时 lock()
   失效则自动清理
   ```

   这样消费者生命周期由真正使用它的模块控制，`CamManager/FrameHub` 不再反向持有业务模块。

10. `CamManager` 引入 return queue。

    `CamManager` 现在不会在 `publishFrame()` 后立刻 `QBUF`，而是：

    ```text
    DQBUF
      -> 创建 FramePacket/FrameLease
      -> publish
      -> 最后一个 lease 释放
      -> postReturnedFrame(cameraId, bufferIndex)
      -> drainReturnedFrames()
      -> requeueFrame/QBUF
    ```

    当前第一版 return queue 已经落地，但还没有接 `eventfd/pipe` 唤醒 `poll()`。代码中已留 TODO：后续应把 return queue 接进 `poll()` 的 fd 集合，避免极端情况下等到 poll timeout 才处理归还。

11. `DisplayConsumer` 改为异步 latest sink。

    第一版 `DisplayConsumer::onFrame()` 虽然已经使用 `FramePacket`，但 RGA 仍然发生在采集线程回调里，本质还是同步显示 sink。为了让多消费者、多摄像头时更容易并发，本次改成：

    ```text
    onFrame(packet)
      -> std::move(packet) 到 m_latestPacket
      -> notify worker
      -> 立刻返回

    workerLoop()
      -> 从 m_latestPacket move 出局部 packet
      -> 清空 latest slot 并释放锁
      -> RGA 到显示私有 DmaBufferPool
      -> packet.lease.reset()
      -> emit frameReady()
    ```

    这里的关键点是：锁只保护 `m_latestPacket` 这个共享槽位，不覆盖 RGA 慢操作。worker 把 packet 从槽位中 `std::move` 到局部变量后，采集线程可以继续塞入新的最新帧。

12. 去掉 `FrameHub` 对 `onFrame()` 的异常捕获。

    `FrameHub` 现在直接调用：

    ```cpp
    consumer->onFrame(packet);
    ```

    不再用 `try/catch` 包住每个 sink。设计约定是：`onFrame()` 不应该抛异常，sink 的错误由自己记录、丢帧或释放私有资源处理。这里没有给接口加 `noexcept`，避免现阶段某个 sink 内部意外异常直接导致 `std::terminate`；后续如果全项目明确禁异常，再统一收紧。

### 重要设计结论

1. 采用路线 A：V4L2 frame lease 异步分发。

   这一版选择让 sink 可以持有 V4L2 frame lease。这样性能和灵活性最好，但要求 sink 自己遵守规则：

   ```text
   如果处理时间可能较长，就尽快 RGA/copy 到自己的 DmaBufferPool，
   然后释放 V4L2 lease。
   ```

2. sink 策略由 sink 自己决定。

   显示 sink 可以只保留最新帧；录像/推流 sink 可以维护自己的 DMA-BUF 队列、MPP 编码队列和丢帧策略。基座只负责生命周期安全，不替业务层决定缓存策略。

3. `AppRuntime` 暂时不落地。

   已经确认未来可以用应用级 `AppRuntime` 持有唯一 `CamManager`，但这一版先不引入，避免把“应用资源组织”和“帧生命周期模型”混在一次改动里。

4. 当前仍需后续收紧。

   后面要重点补：

   - return queue 通过 `eventfd/pipe` 唤醒采集线程。
   - `CamManager` 线程启动/停止和析构清理。
   - 多路摄像头压力测试。
   - 慢 sink 持有 lease 的耗时统计。
   - 录像/推流 sink 的 bounded queue 策略。

### 本次验证

- `qt-demo/build.sh build` 可以完成 aarch64 Qt demo 编译。
- `demo/frame_lease` 可以构建并运行，验证 lease 释放回调模型。
- `demo/test.cpp`、`demo/rga_test.cpp` 已做头文件级编译检查。
- 板端 Qt demo 可稳定输出约 `30fps` 的显示日志。
