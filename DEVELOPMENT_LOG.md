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

## 2026-08-20

### 项目当前进度

本次继续收紧 `FrameLease -> return queue -> QBUF` 这条生命周期链路。

上一版中，`FrameLease` 最后一个引用释放后会调用 `postReturnedFrame(cameraId, bufferIndex)`，把 V4L2 buffer 投递回 `CamManager` 的 return queue。但是采集线程如果正阻塞在 `poll()` 中，可能要等到摄像头 fd 再次就绪或 `poll timeout` 后才会处理归还。

本次给 `CamManager` 增加 `eventfd` 唤醒机制，让消费者线程释放 lease 后可以立刻唤醒采集线程：

```text
FrameLease 析构
  -> CamManager::postReturnedFrame()
  -> push return queue
  -> write(return eventfd)

CamManager::pollOnce()
  -> poll(camera fd + return eventfd)
  -> return eventfd 可读
  -> drainReturnEvent()
  -> drainReturnedFrames()
  -> requeueFrame/QBUF
```

### 今天完成的事

1. `CamManager` 初始化并持有 return eventfd。

   `CamManager` 构造函数中创建：

   ```cpp
   eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)
   ```

   析构函数中关闭 fd。`eventfd` 只作为 `CamManager` 内部跨线程唤醒机制，不暴露给 `V4L2CameraSource`、`FrameHub` 或 sink。

2. `postReturnedFrame()` 接入 eventfd 唤醒。

   现在最后一个 `FrameLease` 释放时，会先把 `(cameraId, bufferIndex)` 放入 return queue，然后调用 `notifyReturnEvent()` 写 eventfd。

   这样异步 sink 完成 RGA/copy 后释放原始 V4L2 lease，不需要等待下一次 `poll timeout`，采集线程会尽快醒来执行 QBUF。

3. `pollOnce()` 同时监听摄像头 fd 和 return eventfd。

   `pollOnce()` 构造 fd 列表时，会额外加入 `m_returnEventFd`。当 eventfd 可读时，先通过 `drainReturnEvent()` 读空 eventfd 计数，再调用 `drainReturnedFrames()` 统一归还 V4L2 buffer。

   `eventfd` 默认不是 `EFD_SEMAPHORE` 模式，一次 `read()` 会读出当前累计计数并清零。这里不依赖计数值决定归还哪些帧，真正的归还信息仍然以 return queue 为准。

4. `requestStop()` / `stopAll()` 也会唤醒 `poll()`。

   停机请求设置 `m_stopRequested` 后会写 eventfd，避免采集线程正阻塞在 `poll()` 时还要等 timeout 才退出。

5. `DisplaySink` 增加 pending 帧丢帧统计。

   显示 sink 仍然使用 latest-slot 策略：

   ```text
   onFrame(packet)
     -> 如果 m_latestPacket 已有旧帧，统计一次 dropped frame
     -> 用新 packet 覆盖旧 packet
     -> notify worker
   ```

   这里统计的是“worker 尚未取走的 pending 旧帧被新帧覆盖”，适合显示场景追最新帧的策略。日志做了节流，避免频繁 `qWarning()` 反过来影响性能。

6. 将 `Consumer` 命名统一迁移为 `Sink`。

   当前架构里这些对象处在 `FrameHub` 下游，职责是接收某一路流并执行显示、RGA copy、编码、推流、AI 等落地处理，语义上更接近 sink。因此本次统一改名：

   ```text
   Consumer           -> Sink
   TestConsumer       -> TestSink
   RgaCopyConsumer    -> RgaCopySink
   DisplayConsumer    -> DisplaySink
   addFrameConsumer   -> addFrameSink
   addConsumerForHub  -> addSinkForHub
   FrameHub::addConsumer -> FrameHub::addSink
   ```

   同时将 `include/consumer`、`src/consumer` 目录改为 `include/sink`、`src/sink`，避免路径上继续残留旧语义。

7. 新增轻量统一调试日志工具。

   新增 `include/Log.hpp`，提供：

   ```cpp
   LOG_TRACE(module, expr)
   LOG_DEBUG(module, expr)
   LOG_INFO(module, expr)
   LOG_WARN(module, expr)
   LOG_ERROR(module, expr)
   ```

   当前日志输出包含：

   ```text
   time [level] [module] [thread-id] file:line function | message
   ```

   第一版先不引入第三方库和异步日志线程，只把核心调试路径统一起来。已替换 `CamManager`、`DisplaySink`、`QtVideoItem`、`RgaCopySink`、`TestSink` 中的散乱 `std::cout/std::cerr/qDebug/qWarning`。demo 中用于展示设备信息的正常输出暂时保留。

   日志支持编译期开关：

   ```bash
   -DLOG_ACTIVE_LEVEL=LOG_LEVEL_WARN  # 只保留 WARN/ERROR
   -DLOG_ACTIVE_LEVEL=LOG_LEVEL_ERROR # 只保留 ERROR
   -DLOG_DISABLE                      # 全部关闭
   ```

### 重要设计结论

1. `eventfd` 解决的是归还及时性，不改变帧生命周期所有权。

   原始 V4L2 buffer 仍然由 `FramePacket::lease` 保护。只有最后一个 `shared_ptr<FrameLease>` 释放后，才会进入 return queue。`eventfd` 只是把“return queue 有新任务”通知给正在 `poll()` 的采集线程。

2. 软件异步不等于 RGA 硬件并行。

   当前 `RgaEngine` 使用同步 `IM_SYNC` 调用。多个 sink worker 可以并发提交 RGA，但在 RK3568 上，RGA2 硬件大概率仍按驱动队列串行执行任务。

   因此性能估算应按：

   ```text
   CamManager 不被 RGA 阻塞
   但同一帧原始 V4L2 lease 的释放时间受所有持有该 lease 的 sink 影响
   多个 RGA job 在 RK3568 上应按硬件队列串行预算
   ```

   例如同一帧被 4 个 sink 分别异步 RGA，每个约 `1.8ms`，则原始 buffer 归还时间可能接近 `1.8ms * 4` 加驱动调度开销，而不是只看最慢一个 sink。

3. 继续坚持 `onFrame()` 快速返回。

   当前基座仍然保持：

   ```text
   CamManager 只负责 poll / DQBUF / publish / QBUF
   FrameHub 只负责同步分发到 sink
   sink 自己决定 latest-slot、有界队列、RGA copy、编码投递和丢帧策略
   ```

   对显示类 sink，可以丢旧帧追最新；对录像/推流类 sink，后续应使用有界队列，并且不能长期持有原始 V4L2 lease。慢处理应先 copy/encode 到 sink 自己的资源，再释放原始 lease。

### 本次验证

- `g++ -std=c++17 -Wall -Wextra -Iinclude -fsyntax-only src/CamManager.cpp` 通过。
- `bash -n build.sh`、`bash -n qt-demo/build.sh`、`bash -n qt-demo/run.sh` 通过。
- Qt 文件的普通主机 `g++ -fsyntax-only` 检查会因为缺少 Qt include 路径失败，需以后以 `qt-demo/build.sh build` 的交叉编译结果为准。
- `./build.sh` 通过，RV1126 32 位 demo 目标均完成构建。
- `./qt-demo/build.sh` 通过，Qt aarch64 demo 完成构建和部署。

## 2026-08-21

### 项目当前进度

本次继续打磨 `CamManager` 的摄像头生命周期边界，重点处理 `pollOnce()` 已经拿到摄像头快照后，外部又调用 `delCamera()` 的情况。

上一版已经用 `eventfd` 解决了 return queue 唤醒问题，但 `CamManager` 内部仍然用 `unique_ptr` 持有 `V4L2CameraSource` 和 `FrameHub`。如果后续支持运行中删除摄像头，`pollOnce()` 里的裸指针快照存在悬空风险。

本次将 camera/hub 快照改为 `shared_ptr` 生命周期保护，并让 `FrameLease` 间接保住对应的 `V4L2CameraSource`，避免异步 sink 还在使用原始 DMA-BUF 时 source 提前析构。

### 今天完成的事

1. `CameraSlot::source` 改为 `std::shared_ptr<V4L2CameraSource>`。

   `CamManager` 仍然通过 `m_cameraMap` 管理 camera，但 `pollOnce()` 不再保存裸指针快照，而是复制 `shared_ptr`：

   ```text
   m_cameraMap
     -> shared_ptr<V4L2CameraSource>
     -> pollOnce() shared_ptr snapshot
   ```

   这样 `delCamera()` 从 map 中移除 camera 后，已经进入本轮 `pollOnce()` 的快照仍然能保证对象活着，不会出现悬空指针。

2. `m_frameHubMap` 改为 `std::shared_ptr<FrameHub>`。

   `pollOnce()` 同时会拿 camera 和 hub 快照。只保护 camera 不够，hub 也可能在 `delCamera()` 时从 map 中移除，因此同步改成 `shared_ptr` 快照。

3. `FrameLease` 捕获 `sourceLifetime`。

   创建 `FramePacket` 时，release callback 现在会捕获当前 camera 的 `shared_ptr`：

   ```cpp
   [this, sourceLifetime = camera, cameraId, bufferIndex]() {
       (void)sourceLifetime;
       postReturnedFrame(cameraId, bufferIndex);
   }
   ```

   这表示只要还有 sink 持有这帧原始 V4L2 lease，对应的 `V4L2CameraSource` 就不会析构。sink 完成 RGA/copy 后释放 lease，source 才允许释放。

4. `publishFrame()` 前增加 active check。

   在 `DQBUF` 之后、发布给 sink 之前，会重新检查当前快照是否仍然是 map 中的 active camera/hub：

   ```text
   cameraId 仍存在
   hub 仍存在
   map 中的 source 仍等于本轮快照 source
   map 中的 hub 仍等于本轮快照 hub
   camera state 仍为 Streaming
   ```

   如果 camera 已经被删除或停止，则跳过本帧发布，立即释放本帧 lease，并尝试 drain return queue。

5. `drainReturnedFrames()` 对已删除 camera 改为跳过。

   return queue 中可能存在已经删除的 cameraId。此时不再把它当作错误中断，而是跳过 QBUF，让 `FrameLease` 捕获的 `sourceLifetime` 在最后释放时带着 `V4L2CameraSource` 析构清理 fd 和 DMA buffer。

6. 将 return queue 中的结构化绑定改成传统写法。

   原写法：

   ```cpp
   const auto [cameraId, bufferIndex] = pending.front();
   ```

   改成更直观的 C++17 以前写法：

   ```cpp
   const std::pair<int, int> item = pending.front();
   const int cameraId = item.first;
   const int bufferIndex = item.second;
   ```

7. 补充 WSL Qt 交叉编译辅助文件。

   新增 `qt-demo/wsl-build.sh` 和 `qt-demo/wsl-toolchain.cmake`，用于 WSL 环境下走 Ninja/CMake 构建 RK3568 aarch64 Qt demo。`.gitignore` 同步忽略 WSL 构建和部署输出目录。

### 重要设计结论

1. `shared_ptr` 快照支持基础运行中删除。

   这一版可以避免 `delCamera()` 导致 `pollOnce()` 快照悬空，也能避免异步 sink 持有原始帧时 source 提前析构。

   当前 `delCamera()` 的语义是从 `m_cameraMap/m_frameHubMap` 摘除管理引用，不主动 `stop()` 快照里的 V4L2 fd。已经在 `pollOnce()` 或 sink 中流转的帧，依靠 `shared_ptr` 快照和 `FrameLease::sourceLifetime` 自然收尾。

2. 删除摄像头的语义变成“停止可见”和“资源最终释放”分离。

   ```text
   delCamera()
     -> 从 map 删除，后续不再进入新快照，也不再 publish 新帧
     -> 已经分发出去的 FrameLease 继续保护原始 buffer
     -> 最后一个 lease 释放后，sourceLifetime 释放，source 析构清理资源
   ```

   这会让资源释放延迟到最后一个原始 lease 释放，但这是为了保证异步 sink 不读到已经释放的 DMA-BUF。

3. `onFrame()` 仍然必须快速返回。

   如果 sink 长时间持有原始 V4L2 lease，删除摄像头和 buffer 回收都会被拖慢。录像/推流/AI 等慢处理后续应先 copy/encode 到自己的资源，再释放原始 lease。

4. 当前不要在旧 lease 完全释放前复用同一个 `cameraId`。

   现在 return queue 里保存的是：

   ```cpp
   cameraId
   bufferIndex
   ```

   如果 `delCamera(0)` 后旧 camera 的 lease 还没完全释放，又立刻 `addCamera(0)`，旧 lease 的归还事件可能和新 cameraId 混淆。

   后续计划将 cameraId 改为内部自动生成并返回给上层，或者给 cameraId 增加 generation 校验，避免旧归还事件误投到新摄像头。

### 本次验证

- `g++ -std=c++17 -Wall -Wextra -Iinclude -Iinclude/hw -Iinclude/sink -Ithird_party/rga/include -fsyntax-only src/CamManager.cpp src/FrameHub.cpp src/DmaBufferPool.cpp src/sink/RgaCopySink.cpp demo/test.cpp demo/rga_test.cpp` 通过。
- `bash -n build.sh`、`bash -n qt-demo/build.sh`、`bash -n qt-demo/run.sh`、`bash -n qt-demo/wsl-build.sh` 通过。

## 2026-08-22

### 项目当前进度

本次继续收紧 `CamManager` 基座的线程生命周期和摄像头控制语义。上一版已经通过 `shared_ptr` 快照和 `FrameLease::sourceLifetime` 解决了运行中删除摄像头时的对象生命周期问题，本次重点处理另一个维度：`STREAMON/STREAMOFF`、`DQBUF/QBUF` 不应在多个线程同时操作同一个 V4L2 fd。

当前摄像头基座语义调整为：

```text
addCamera/delCamera
  -> 仍保持 shared_ptr 管理引用语义

startCamera/stopCamera
  -> 外部只投递内部命令
  -> 由 CamManager poll 线程统一执行 STREAMON/STREAMOFF

FrameLease 释放
  -> 投递 return queue
  -> CamManager poll 线程统一 QBUF
```

### 今天完成的事

1. 拆分 `CamManager` 线程生命周期和摄像头流生命周期命名。

   线程生命周期接口改为：

   ```cpp
   startPolling();
   shutdownPolling();
   ```

   摄像头流生命周期接口改为：

   ```cpp
   startCamera(cameraId);
   stopCamera(cameraId);
   startAllCameras();
   stopAllCameras();
   ```

   这样 `CamManager` 自己的 poll 线程和 camera 的 `STREAMON/STREAMOFF` 不再混在 `start/stop/startAll/stopAll` 这类含糊名字里。

2. 给 `CamManager` 增加内部命令队列。

   新增内部命令：

   ```cpp
   StartCamera
   StopCamera
   ```

   外部调用 `startCamera()` / `stopCamera()` 时只负责投递命令并唤醒 poll 线程；真正的 `camera.start()` / `camera.stop()` 在 `drainCommands()` 中执行。

   这里没有把 `delCamera()` 放进命令队列，因为当前删除语义是从 map 摘除 `shared_ptr` 管理引用，不主动 `STREAMOFF`，仍然依靠 `shared_ptr` 快照和 `FrameLease` 保证旧帧自然收尾。

3. 修正 `CamManager` 后台线程生命周期。

   `CamManager` 析构时会先 `shutdownPolling()`，再关闭 `eventfd`，避免后台线程仍在 poll/read eventfd 时对象已经析构。

   `m_stopRequested` / `m_running` 改为 atomic，`startPolling()` 使用 `exchange(true)` 防止重复启动。`m_pollThread` 的 join/重建由 `m_threadMutex` 保护。

4. 处理无摄像头时的等待和唤醒。

   `pollOnce()` 在没有 Streaming camera 时不再直接报错退出，而是通过 `condition_variable` 等待：

   ```text
   有 camera 进入 Streaming
   或收到内部 command
   或请求 shutdown
   ```

   这样可以先启动 poll 线程，再按需添加/启动摄像头。

5. `lastError()` 改为线程安全返回拷贝。

   `m_lastError` 增加独立 mutex。`lastError()` 不再返回 `const std::string&`，而是返回 `std::string` 拷贝，避免后台 poll 线程写错误信息时，其他线程同时读内部字符串引用。

6. 支持 `stopCamera()` 后再次 `startCamera()`。

   `V4L2CameraSource::stop()` 在 `STREAMOFF` 后会把所有 buffer 标记为未 queued。为支持恢复采集，`V4L2CameraSource::start()` 现在会在 `STREAMON` 前重新 QBUF 所有未 queued 的 buffer。

   流程变为：

   ```text
   stopCamera()
     -> STREAMOFF
     -> buffer.queued = false

   startCamera()
     -> QBUF 未 queued buffer
     -> STREAMON
   ```

7. stop 后旧 `FrameLease` 归还不再 QBUF。

   如果某帧已经发布给 sink，随后外部调用 `stopCamera()`，旧 lease 之后释放时仍会进入 return queue。此时 `drainReturnedFrames()` 会检查 camera 是否仍为 Streaming；如果已经停止，则跳过 QBUF。

   这样避免在 `STREAMOFF` 后调用 `requeueFrame()`，下一次 `start()` 会重新 QBUF 全部 buffer。

8. 同步更新 demo 调用名。

   `demo/test.cpp`、`demo/rga_test.cpp`、`demo/cam_manager_demo.cpp`、`qt-demo/myitem.cpp` 中的旧接口名同步调整为 `startAllCameras()` / `stopAllCameras()`。

### 重要设计结论

1. `shared_ptr` 和内部命令队列解决的是两类问题。

   ```text
   shared_ptr / FrameLease:
     解决对象和 DMA buffer 生命周期

   内部命令队列:
     解决 V4L2 fd 状态机操作时序
   ```

   二者不是互相替代关系，当前基座会同时保留。

2. `startCamera()` / `stopCamera()` 当前返回的是“命令投递成功”。

   因为真正 `STREAMON/STREAMOFF` 在 poll 线程执行，所以外部接口返回 `true` 不表示硬件已经完成启动或停止。后续如果需要同步知道执行结果，可以再给 command 增加结果回传或回调。

3. `delCamera()` 仍保持当前删除语义。

   当前 `delCamera()` 不主动 stop，也不进入命令队列；它只从 `m_cameraMap/m_frameHubMap` 中摘除管理引用。已经被 poll 线程或 sink 持有的旧帧继续依靠 `shared_ptr` 快照和 `FrameLease::sourceLifetime` 收尾。

4. Qt demo 仍未收口。

   当前 Qt demo 仍然在 `MyItem` 内部创建 `CamManager` 并启动线程，生命周期还没有接入正式 `AppRuntime`。下一步应把 Qt 显示路径拆成 runtime 管理，`MyItem` 只负责显示。

### 本次验证

- `g++ -std=c++17 -Wall -Wextra -Iinclude -Iinclude/hw -Iinclude/sink -Ithird_party/rga/include -fsyntax-only src/V4L2CameraSource.cpp src/CamManager.cpp src/FrameHub.cpp src/DmaBufferPool.cpp src/sink/RgaCopySink.cpp demo/test.cpp demo/rga_test.cpp demo/cam_manager_demo.cpp` 通过。
- 当前仅剩 `V4L2CameraSource.cpp` 中 `PixelFormat::RGBA8888` 未覆盖 switch 的旧 warning，与本次基座变更无关。

## 2026-08-25

### 项目当前进度

本次把前面计划中的应用级运行上下文 `AppRuntime` 真正落地，并将 Qt demo 里的摄像头管理从 `MyItem` 中移出。

上一版日志里还记录为：

```text
Qt demo 仍然在 MyItem 内部创建 CamManager 并启动线程，
生命周期还没有接入正式 AppRuntime。
```

当前代码已经调整为：

```text
AppRuntime
  -> 持有唯一 CamManager

qt-demo/main.cpp
  -> 从 AppRuntime 获取 CamManager
  -> addCamera()
  -> startAllCameras()
  -> startPolling()
  -> aboutToQuit 时 shutdownPolling()

MyItem
  -> 不再创建 CamManager
  -> 只从 AppRuntime 获取 CamManager
  -> 注册自己的 DisplaySink
  -> 负责 dmaFd -> EGLImage -> QSGTexture 显示
```

这一步把“应用运行资源管理”和“QML Item 视觉显示职责”分开了。`MyItem` 不再自己起摄像头线程，也不再拥有摄像头管理器；它只作为显示控件存在。

### 今天完成的事

1. 新增 `AppRuntime` 单例。

   新增：

   ```text
   include/AppRuntime.hpp
   src/AppRuntime.cpp
   ```

   第一版 `AppRuntime` 只持有：

   ```cpp
   CamManager m_camManager {};
   ```

   并通过：

   ```cpp
   CamManager& getCamManager() noexcept;
   ```

   向应用层提供唯一的摄像头管理器。

2. Qt demo 的摄像头初始化迁移到 `main.cpp`。

   `qt-demo/main.cpp` 现在负责：

   ```text
   创建 QGuiApplication
   获取 AppRuntime::getInstance().getCamManager()
   配置 /dev/video10 640x480 30fps YUYV 摄像头
   加载 QML
   startAllCameras()
   startPolling()
   ```

   同时通过 `QCoreApplication::aboutToQuit` 连接：

   ```cpp
   camManager.shutdownPolling();
   ```

   这样 Qt 应用退出时会显式停止 CamManager 的后台 poll 线程。

3. `MyItem` 不再创建 `CamManager`。

   旧逻辑中，`MyItem` 构造函数里会起一个线程，在里面 new `CamManager`、add camera、add sink、run。这个职责太重，也会让一个显示控件偷偷拥有全局摄像头运行逻辑。

   当前 `MyItem` 只做：

   ```text
   创建/持有自己的 DisplaySink
   从 AppRuntime 获取 CamManager
   addFrameSink(0, m_displaySink)
   连接 frameReady/displayFrameDone 信号
   ```

   这让 `MyItem` 的语义更清晰：它是“显示某一路 stream 的 Qt Quick Item”，不是应用运行入口。

4. `CamManager::pollOnce()` / `run()` 收回为私有接口。

   最新代码中，外部不再直接调用：

   ```cpp
   pollOnce()
   run()
   ```

   而是通过：

   ```cpp
   startPolling()
   shutdownPolling()
   ```

   管理采集线程生命周期。这样可以避免外部随意在当前线程里跑 `run()`，也更符合 `CamManager` 后台 poll 线程的设计。

5. Qt demo 构建接入 `AppRuntime.cpp`。

   `qt-demo/CMakeLists.txt` 已加入：

   ```text
   ../src/AppRuntime.cpp
   ```

   保证 Qt demo 链接到新的应用运行上下文实现。

### 重要设计结论

1. `AppRuntime` 是应用级资源所有者，不是业务 pipeline。

   当前 `AppRuntime` 只先放 `CamManager`，后续如果需要，可以继续收纳：

   ```text
   StreamHub / Sink 注册表
   硬件模块上下文
   应用配置
   Qt 显示侧全局状态
   ```

   但它不应该变成一个大而乱的处理节点。真正的视频处理策略仍然放在 sink / hw 模块 / 后续 pipeline 节点里。

2. `MyItem` 只负责显示，不负责启动摄像头。

   这是 Qt Quick 路径里比较重要的边界。QML Item 的生命周期可能受界面创建/销毁影响，如果摄像头采集也藏在 Item 内部，后续多画面、切换页面、全屏/缩略图重排都会变得很难控。

   现在摄像头运行跟随应用，显示控件只订阅并显示，这个方向更适合后续多路视频。

3. 当前仍是第一版 AppRuntime。

   这一版先解决 ownership 和入口问题，还没有做：

   - 多路摄像头配置加载。
   - 多个 `MyItem` 按 streamId 动态订阅。
   - Sink 重复注册/注销。
   - AppRuntime 析构时统一停止所有硬件资源。

   后续 Qt 多路显示时，`MyItem` 应该支持配置自己的 `streamId`，而不是固定订阅 `0`。

### 本次代码差异

最近两次提交：

```text
0810d70 准备程序运行上下文单例类
2a34dbe 将摄像头管理器交由app运行环境类管理
```

主要涉及：

```text
include/AppRuntime.hpp
src/AppRuntime.cpp
include/CamManager.hpp
qt-demo/CMakeLists.txt
qt-demo/main.cpp
qt-demo/myitem.cpp
qt-demo/myitem.h
```

### 本次验证

- 当前工作区 `git status --short` 为空，说明日志补充前代码处于干净提交状态。
- 已阅读最新提交差异，确认 `CamManager` 已由 `AppRuntime` 持有，Qt demo 的摄像头启动已经从 `MyItem` 迁移到 `main.cpp`。

## 2026-08-28

### 项目当前进度

本次主要推进 Qt 显示链路从 demo 形态向 NVR 预览形态靠拢，同时继续验证摄像头基座的真实性能瓶颈。当前结论比较明确：两路 USB UVC 摄像头下，用户态显示链路不是最大压力，最大压力来自 `uvcvideo` 内核线程把 URB 数据 memcpy 到 vb2 buffer。

### 今天完成的事

1. Qt demo 接入 `AppRuntime` 和 `DisplayController`。

   `DisplayController` 作为 QML 和 C++ 基座之间的轻量控制入口，负责从 `AppRuntime` 获取 `CamManager`，启动 polling，并提供：

   ```cpp
   addLocalCam(path)
   startLocalCam(cameraId)
   ```

   `MyItem` 不再在构造时写死绑定 camera 0，而是通过 `cameraId` 属性绑定具体摄像头。

2. `CamManager::addCamera()` 改为内部生成 cameraId。

   外部不再传入 cameraId，`CamManager` 使用单调递增 int 生成 id，并把成功生成的 id 返回给上层。这样可以避免外部复用 cameraId 时，旧 `FrameLease` 归还事件误命中新摄像头。

   当前语义为：

   ```cpp
   int addCamera(const CameraConfig& config);
   ```

   返回 `>= 0` 表示 cameraId，返回 `-1` 表示失败，错误信息通过 `lastError()` 获取。

3. Qt demo 支持 NVR 风格多格预览和拖拽换位。

   QML 使用 `Repeater` 生成 6 个视频格子，通过 `slot` 和 `geometryForSlot()` 控制 2 路、4 宫格、6 路布局。拖拽时只交换 `slot`，QML 绑定会自动更新位置和大小。

4. 视频显示改为等比例完整显示。

   `MyItem::updatePaintNode()` 不再把纹理直接拉伸到整个格子，而是按原始 `frame.width/frame.height` 计算居中目标矩形。这样画面不会变形，空出来的区域由外层 tile 显示为纯黑。

5. 修正 Qt Scene Graph 空纹理节点风险。

   `updatePaintNode()` 在没有有效帧时不再创建空的 `QSGSimpleTextureNode`。只有拿到有效 `DisplayFrame` 后才创建/更新 texture node，避免 Qt 渲染线程拿到无 texture node 后出现崩溃。

6. 当前 demo 为测试两路摄像头，启动时自动打开两路。

   临时测试路径为：

   ```qml
   /dev/video12
   /dev/video10
   ```

   这是为了不依赖鼠标点击，方便在板端直接观察两路摄像头的 CPU 和显示消耗。后续正式 UI 会改为设备列表/按钮选择。

### UVC 性能实测

本次在 RK3568 板端重新打开 kernel function profiler：

```text
CONFIG_KALLSYMS_ALL=y
CONFIG_FTRACE=y
CONFIG_FUNCTION_TRACER=y
CONFIG_FUNCTION_PROFILER=y
```

然后运行两路 USB UVC 摄像头：

```text
/dev/video10  640x480 YUYV 30fps
/dev/video12  640x480 YUYV 30fps
```

理论输入数据量：

```text
单路: 640 * 480 * 2 * 30 = 18.4 MB/s
两路: 约 36.9 MB/s
```

内核源码路径已经确认：

```text
uvc_video_complete()
  -> stream->decode()
  -> uvc_video_decode_data()
  -> queue_work(stream->async_wq, &uvc_urb->work)
  -> uvc_video_copy_data_work()
  -> memcpy(op->dst, op->src, op->len)
```

也就是说 UVC 摄像头的数据不是直接进最终 vb2 buffer，而是 USB 控制器先把数据放进 URB buffer，随后 `uvcvideo` 的 worker 线程把 URB payload memcpy 到 vb2 video buffer。

10 秒 function profiler 聚合结果：

```text
uvc_video_copy_data_work        9.16s / 10s  约 91.6% 单核，约 22.9% 四核
uvc_video_complete              1.23s / 10s  约 12.3% 单核，约  3.1% 四核
usb_submit_urb                  0.92s / 10s  约  9.2% 单核，约  2.3% 四核
ehci_irq                        0.42s / 10s  约  4.2% 单核，约  1.1% 四核
rga_ioctl                       1.37s / 10s  约 13.7% 单核，约  3.4% 四核
rga_request_wait                0.83s / 10s  约  8.3% 单核，约  2.1% 四核
```

注意：`rga_request_wait`、`drm_atomic_helper_wait_for_vblank` 这类大量时间主要是等待硬件完成或等待垂直同步，不能简单当作 CPU 忙算。

### 重要设计结论

1. 当前两路 USB UVC YUYV 最大成本在内核 memcpy。

   两路 640x480 YUYV 30fps 时，`uvc_video_copy_data_work` 接近吃满一个 A55 核。也就是说，如果继续用 USB UVC 原始 YUYV 流，多路扩展时这块会比当前用户态基座更早成为瓶颈。

2. 当前 C++ 基座方向是成立的。

   `CamManager + FrameHub + Sink + FrameLease` 这条链路没有暴露出明显的性能瓶颈。用户态 `appqt-demo` 进程约 0.37 个核，UVC memcpy 约 0.92 个核，说明真正重头在 USB UVC 内核搬运。

3. 如果换成两路 MIPI，CPU 占用有机会显著下降。

   MIPI/CSI 路径通常不需要 UVC 这种 URB 到 vb2 的 CPU memcpy，数据可以更直接地进入视频 buffer。所以两路 MIPI 在相同显示链路下，有机会把总 CPU 拉到十几到二十以内这一档，具体仍需要板端实测。

4. 另一个可行方向是 USB MJPEG + MPP 解码。

   如果摄像头输出 MJPEG，USB 传输数据量会大幅下降，也能减少 UVC memcpy 的字节量。后续可以新增压缩流/解码链路，用 MPP 解码后再进入统一 hub/sink 体系。不过当前解码模块还没做，先不提前侵入现有摄像头基座。

### 本次验证

- `./wsl-build.sh build` 通过，生成 `qt-demo/build-wsl-aarch64/appqt-demo`。
- 板端运行两路 `/dev/video12`、`/dev/video10` 可启动。
- 板端 kernel function profiler 已用于 UVC 拷贝占用测试，测试后已关闭 profiler。

## 2026-08-29

### 项目当前进度

今天主要做两件事：把本地摄像头能力查询补成一个独立 probe 工具，同时开始明确后续压缩流 / MPP 解码链路的架构边界。当前判断是：摄像头侧裸帧基座已经基本稳定，下一步真正关键的是把 MJPEG / H264 / H265 这类压缩输入解码成统一 `VideoFrame`，再复用现有 `FrameHub + Sink`。

### 今天完成的事

1. `DisplaySink` 改为按第一帧真实尺寸初始化显示 pool。

   旧逻辑在构造函数里按固定尺寸预分配 RGBA DMA buffer。这样在 640x480、640x360 或其他测试分辨率下会浪费内存，也让 sink 和摄像头配置耦合。

   当前逻辑改为：

   ```text
   DisplaySink 构造
     -> 只启动 worker

   第一帧进入 processFrame()
     -> 读取 frame.width / frame.height
     -> 计算 RGBA8888 目标 buffer size
     -> 初始化 DmaBufferPool
     -> 后续同尺寸帧复用 pool
   ```

   如果同一个 `DisplaySink` 后续收到不同尺寸帧，当前先打印 warning 并丢弃。热切分辨率后续按“销毁旧 sink / 创建新 sink”的实例生命周期处理，不在已有 sink 内部重置 pool，避免 Qt 渲染线程、RGA worker、in-flight DMA buffer 之间的同步复杂度。

2. 新增非侵入式 V4L2 设备能力查询。

   新增：

   ```text
   include/V4L2DeviceProbe.hpp
   src/V4L2DeviceProbe.cpp
   demo/v4l2_probe_demo.cpp
   ```

   第一版 probe 只做非侵入查询，不主动 `REQBUFS/STREAMON/DQBUF`，避免抢占正在运行的摄像头。当前查询内容包括：

   ```text
   VIDIOC_QUERYCAP
   VIDIOC_G_FMT
   VIDIOC_ENUM_FMT
   VIDIOC_ENUM_FRAMESIZES
   VIDIOC_ENUM_FRAMEINTERVALS
   ```

   过滤条件为：

   ```text
   必须支持 VIDEO_CAPTURE 或 VIDEO_CAPTURE_MPLANE
   必须支持 STREAMING
   排除 META_CAPTURE
   必须能枚举出至少一种格式
   ```

3. 新增 `v4l2_probe_demo` 构建入口。

   `build.sh` 已加入 `v4l2_probe_demo`，用于板端快速列出可用视频节点、当前格式、支持分辨率和 fps。

4. Qt demo 测试配置调整。

   当前 `DisplayController` 默认配置临时改为：

   ```text
   640x360 YUYV 5fps
   ```

   `Main.qml` 默认自动打开顺序为：

   ```text
   /dev/video10
   /dev/video12
   ```

   这是为了继续做两路摄像头压测和 UI 显示验证，后续正式 UI 会改为从 probe 结果选择设备和分辨率。

### V4L2 Probe 板端验证

板端运行 `v4l2_probe_demo` 后，USB 摄像头节点识别正常：

```text
/dev/video10  uvcvideo  current: 640x480 YUYV
/dev/video11  is not a usable video capture node
/dev/video12  uvcvideo  current: 640x360 YUYV
```

其中 `/dev/video11`、`/dev/video13` 是 UVC metadata 节点，不应该作为摄像头采集入口。

同时也确认 RKISP 节点的特殊性：

```text
/dev/video0  rkisp_mainpath  current: unconfigured
/dev/video1  rkisp_selfpath  current: unconfigured
/dev/video2  rkisp_rawwr0    current: unconfigured
```

这些节点能枚举格式，不代表一定有 sensor 真实接入并能出帧。对 RK MIPI / ISP，稳妥 probe 需要额外结合 media graph 或显式试采一帧。

后续更完整的“确定可出帧”策略应分三层：

```text
1. QUERYCAP：筛掉 metadata / output / 非 streaming 节点
2. ENUM_FMT/SIZE/FPS：拿到能力列表
3. 可选强验证：REQBUFS + STREAMON + DQBUF 试取一帧
```

USB UVC 一般前两层就够用；RKISP/MIPI 最好增加第三层，或者解析 media graph 中是否存在真实 Sensor entity 且链路闭合。

### MIPI 摄像头设备树结论

当前板端实际启动的模型为：

```text
Alientek ATK-DLRK3568 Board
compatible: rockchip,rk3568-evb1-ddr4-v10
```

当前 ATK 设备树里写过这些 MIPI sensor 节点：

```text
sony,imx335    module: MTV4-IR-E-P
sony,imx415    module: CMK-OT1522-FG3
ovti,ov13850   module: ZC-OV13850R2A-V1
```

SDK 里另有 `rk3568-evb1-dual-camera.dtsi` 双摄参考：

```text
galaxycore,gc2053
galaxycore,gc2093
```

但当前运行中的 media graph 没有看到具体 MIPI sensor entity 接进 RKISP，说明现有板端镜像虽然有 RKISP video 节点，但没有实际 MIPI 摄像头链路闭环。后续如果买 MIPI 模组，优先买和 ATK / 正点原子板卡配套的完整模组，而不是只看同 sensor 名称。

### MPP 解码架构决策

阅读 `/home/hjy/rockchip_hardware_acceleration` 后确认：

```text
mpp_simple:
  适合 H264/H265 流式解码。
  MJPEG 不走 simple 模式。

mpp_advance:
  适合 USB 摄像头 MJPEG 单帧解码。
  输入是一帧 MJPEG dma-buf fd。
  输出是外部提供的 NV12 dma-buf fd。
```

后续架构方向：

```text
CamManager MJPEG / RtspStream H264/H265
  -> DecodeHub / DecodeNode
      -> 持有 MppDecoder
      -> 持有解码输出 DmaBufferPool
      -> 解码成裸 VideoFrame
      -> publish 到裸帧 FrameHub
  -> DisplaySink / RecordSink / AISink
```

这里的关键边界是：

```text
FrameHub 继续保持纯裸帧分发
DisplaySink 继续只负责显示裸帧
MppDecoder 放在 DecodeHub / DecodeNode 内部
```

这样后续 USB MJPEG、RTSP H264/H265、IPC 子码流都能统一成 `VideoFrame + FrameLease` 后进入同一套 sink 体系。

### 本次验证

- `g++ -std=c++17 -Wall -Wextra -Iinclude -fsyntax-only src/V4L2DeviceProbe.cpp demo/v4l2_probe_demo.cpp` 通过。
- 使用 RK3568 aarch64 工具链编译 `build/v4l2_probe_demo` 通过。
- 板端运行 `/tmp/v4l2_probe_demo` 能正确列出 `/dev/video10`、`/dev/video12` 的格式和分辨率，并过滤 UVC metadata 节点。

### 追加：MJPEG 格式接入与动态删除语义收紧

1. `PixelFormat` 增加 `MJPEG` 后，补齐了 V4L2 采集链路中的真实格式映射。

   当前 `V4L2CameraSource::configure()` 显式请求 MJPEG 时会执行：

   ```text
   PixelFormat::MJPEG
     -> V4L2_PIX_FMT_MJPEG
     -> VIDIOC_S_FMT
     -> VIDIOC_G_FMT 回读校验 fourcc
   ```

   如果驱动没有接受 MJPEG，而是自动退回其他格式，配置会失败，不会静默变成 YUYV/NV12。`VideoFrame::bytesUsed` 用来表示当前 MJPEG 压缩包的真实长度，`capacity/sizeimage` 仍表示 V4L2 buffer 的最大容量。

2. `V4L2DeviceProbe` 和 demo 工具补齐 MJPEG 识别。

   `v4l2_probe_demo` 现在能把 `MJPG` 标成 `MJPEG`。`camera_capture_demo` 增加可选 `format` 参数，便于板端直接验证 MJPEG 采集：

   ```bash
   ./build/camera_capture_demo /dev/video10 1920 1080 30 5 "" mjpeg
   ```

   当前板端实测 `/dev/video10`、`/dev/video12` 都能枚举出 `MJPG`，但测试时设备被占用，`VIDIOC_S_FMT` 返回 `Device or resource busy`。

3. RGA 明确拒绝 MJPEG 压缩格式。

   MJPEG 不是裸帧，不能直接送给 RGA 做颜色转换或缩放。当前 `RgaEngine` 遇到 `PixelFormat::MJPEG` 会返回明确错误：

   ```text
   RGA 不支持 MJPEG 压缩格式，需要先解码成裸帧
   ```

   这保证后续在 MPP 解码链路完成前，Qt 显示侧不会误把压缩包当裸图像处理。

4. 收紧 `delCamera()` 后的发布语义。

   之前 `pollOnce()` 会复制 `camera/hub` 的 `shared_ptr` 快照。这样虽然对象生命周期安全，但存在一个业务窗口：

   ```text
   poll 线程拿到旧 hub 快照
   外部 delCamera() 删除 map
   poll 线程继续 publish 最后一帧
   ```

   当前给 `FrameHub` 增加关闭闸门：

   ```text
   delCamera()
     -> CameraState::Deleting
     -> FrameHub::close()
        -> 等待正在进行的 publishFrame() 结束
        -> 清空 sink
        -> 后续 publishFrame() 直接丢弃
     -> 投递 DeleteCamera 命令
     -> poll 线程串行 erase camera/hub
   ```

   因此当前删除语义为：

   ```text
   delCamera() 调用期间，已经开始发布的一帧允许完成；
   delCamera() 返回之后，对应 hub 不再向 sink 发布新帧。
   ```

   这个语义依赖所有 sink 的 `onFrame()` 快速返回。慢操作仍然应该放到 sink 自己的 worker 线程里。

### 追加验证

- `git diff --check` 通过。
- `g++ -std=c++17 -Wall -Wextra -Iinclude -fsyntax-only src/FrameHub.cpp src/CamManager.cpp src/V4L2CameraSource.cpp` 通过。
- RK3568 aarch64 工具链手工编译 `build/v4l2_probe_demo`、`build/camera_capture_demo` 通过。
- `cmake --build qt-demo/build-wsl-aarch64 -j$(nproc)` 通过。

### 追加封装mpp解码器
	有以解码能力
- mjpeg
- h264/h265
> 测试demo demo/mpp_decoder_demo.cpp，测试通过

### 追加：MPP 解码器与编码器基础封装

本次把已经验证过的 Rockchip MPP C 代码接入工程，并在 C++ 层做最小封装。

#### 1. MPP C 适配层

新增：

```text
include/hw/rkmpp_c/mpp_simple.h
include/hw/rkmpp_c/mpp_advance.h
src/hw/rkmpp_c/mpp_simple.c
src/hw/rkmpp_c/mpp_advance.c
```

当前分工：

```text
mpp_advance:
  用于 MJPEG 单帧解码。
  输入 MJPEG dma-buf fd，输出外部提供的 NV12 dma-buf fd。

mpp_simple:
  用于 H264/H265 流式解码和 H264/H265 编码。
```

H264/H265 decoder 输入缓存从固定 4MB 改成按需扩容，去掉了 `RKMPP_DEC_INPUT_BUF_SIZE` 这个硬上限，也避免每包 `memset 4MB`。

#### 2. MppDecoder 封装收窄

`MppDecoder` 对外只保留必要接口：

```cpp
bool init(MppCodec codec);
bool decodeMjpeg(const VideoFrame& input, VideoFrame& output);
bool sendPacket(const VideoFrame& packet, bool eos = false);
void setFrameCallback(FrameCallback callback);
```

删除了 public `MppDecConfig` / `MppDecodedInfo` 这类配置结构。解码器不暴露宽高、fps、bitrate 配置；H264/H265 的实际输出宽高、stride、format 由码流和 MPP info_change 决定。

底层解码输出处增加 10s 限流日志，打印输出尺寸、stride、format、fd、buffer size、pts，并尝试用 `MPP_DEC_QUERY` 输出 runtime fps/bps 统计。

#### 3. MppEncoder 基础封装

新增 `MppEncoder`：

```cpp
bool init(const MppEncoderConfig& config);
bool sendFrame(const VideoFrame& frame, bool eos = false);
bool requestKeyFrame();
void setPacketCallback(PacketCallback callback);
```

第一版只支持：

```text
NV12 dma-buf -> H264/H265 packet
```

也就是说，YUYV/RGBA/MJPEG 都不直接送 encoder。后续录像/推流链路应该先通过 RGA/MPP 解码统一成 NV12，再送 MPP encoder，避免让 encoder 内部隐式做颜色转换，便于控制带宽和性能。

`writeHeader()` 没有暴露给上层，编码器在第一次 `sendFrame()` 前内部自动写 header。底层仍设置：

```text
MPP_ENC_HEADER_MODE_EACH_IDR
```

因此每个 IDR 前会携带 VPS/SPS/PPS 或 SPS/PPS，适合 RTSP/NVR 场景里客户端中途恢复解码。

#### 4. 强制关键帧

`MppEncoder::requestKeyFrame()` 底层调用：

```text
MPP_ENC_SET_IDR_FRAME
```

RK MPP 注释语义为“下一帧编码成 intra frame”。`EncodedPacket` 增加 `isKeyFrame` 字段，来自 MPP packet meta：

```text
KEY_OUTPUT_INTRA
```

这方便后续 RTSP/录像层确认关键帧边界。

#### 5. 编码 demo

新增：

```text
demo/mpp_encoder_demo.cpp
```

用法示例：

```bash
./build/mpp_encoder_demo h264 input.nv12 out.h264 640 480 640 480 30 1000000 20 10
./build/mpp_encoder_demo h265 input.nv12 out.h265 640 480 640 480 30 1000000 20 10
```

含义：

```text
编码 20 帧
GOP 固定为 100
在第 10 帧前调用 requestKeyFrame()
```

#### 6. 板端验证

RK3568 板端已验证：

```text
MJPEG -> NV12
NV12 -> H264 -> NV12
NV12 -> H265 -> NV12
```

关键帧请求验证：

```text
H264:
  第 0 帧 keyFrame=1
  第 1-9 帧 keyFrame=0
  第 10 帧前 requestKeyFrame()
  第 10 帧 keyFrame=1
  后续继续 keyFrame=0

H265:
  同样生效
```

额外使用 `ffprobe` 验证，GOP=100 时码流开头为 I 帧，中途 request 后再次出现 I 帧，后续继续 P 帧。

#### 7. 注意点

`MppEncoderConfig::heightStride` 保留。它对应 MPP encoder 的 `prep:ver_stride`，不是码控参数，但它描述输入 NV12 dma-buf 的真实内存布局。调用方不填时按 `height` 处理。

H265 解码输出时 MPP 可能返回比 width 更大的 stride，例如 640x480 输入，输出 stride 可能为 768x480。后续所有 RGA/编码链路都必须按 `stride/heightStride` 访问内存，按 `width/height` 表示真实画面。

### 追加：编码器码率档位与 demo 修正

本次继续打磨 MPP 编码器封装，重点是让编码参数更适合后续 IPC / RTSP / NVR 场景，同时修正编码 demo 的测试问题。

#### 1. 修正 mpp_encoder_demo 只编码单帧的问题

之前 `demo/mpp_encoder_demo.cpp` 有两个测试坑：

```text
1. 默认 frameCount=1，不传帧数时只编码一帧。
2. 输入 DMA buffer 只在循环前 memcpy 一次，即使传多帧，也会重复编码第一帧。
```

本次改为：

```text
1. 按 NV12 文件大小自动计算可用帧数。
2. 如果外部传 frameCount，则取外部帧数和文件实际帧数的较小值。
3. 每次 sendFrame() 前将对应帧拷贝到输入 dma-buf。
4. memcpy 前后增加 DMA_BUF_IOCTL_SYNC，保证 CPU 写入和设备读取之间的同步语义更明确。
```

修正后板端用 `ffprobe -count_frames` 验证：

```text
H264: 640x480 30fps 150 frames
H265: 640x480 30fps 150 frames
```

确认不再是只编码单帧。

#### 2. 增加 RK3568 WSL 构建脚本

新增：

```text
wsl-build.sh
```

用于直接使用：

```text
/opt/rk3568_kernel_pack/toolchain
/opt/rk3568_kernel_pack/sysroot
```

构建当前几个板端 demo：

```text
mpp_decoder_demo
mpp_encoder_demo
v4l2_probe_demo
camera_capture_demo
```

这避免继续误用旧的 RV1126 `build.sh`。

#### 3. 增加编码码率档位

`MppEncoderConfig` 新增：

```cpp
enum class MppBitratePreset {
    Low,
    Medium,
    High,
    VeryHigh,
};
```

规则：

```text
bitrate > 0:
  使用外部传入的精确码率，单位 bit/s。

bitrate <= 0:
  按 bitratePreset 自动计算。
```

自动码率公式：

```text
base = width * height * fps / 8

H264 Low      = base * 2 / 3
H264 Medium   = base
H264 High     = base * 3 / 2
H264 VeryHigh = base * 2

H265 Low      = H264 Low * 65%
H265 Medium   = H264 Medium * 65%
H265 High     = H264 High * 65%
H265 VeryHigh = H264 VeryHigh * 65%
```

以 `640x480@30fps` 为例：

```text
H264 Medium = 1,152,000 bit/s
H265 Medium =   748,800 bit/s
```

这样上层后续可以传 `Low / Medium / High / VeryHigh` 这种语义档位，不必每次手填裸码率。专业配置仍然可以直接填精确 `bitrate`。

#### 4. demo 支持码率档位参数

`mpp_encoder_demo` 第 9 个参数现在既可以传精确码率，也可以传档位：

```bash
./build/mpp_encoder_demo h264 input.nv12 out.h264 640 480 640 480 30 medium
./build/mpp_encoder_demo h265 input.nv12 out.h265 640 480 640 480 30 high
./build/mpp_encoder_demo h264 input.nv12 out.h264 640 480 640 480 30 1500000
```

板端验证 `640x480@30fps medium`：

```text
H264 bitrate = 1,152,000 bit/s
H265 bitrate =   748,800 bit/s
```

短测 10 帧输出：

```text
H264 输出约 83KB
H265 输出约 43KB
```

#### 5. 编码参数笔记

新增：

```text
docs/mpp_encoder_params_notes.md
```

记录当前对 MPP 编码参数的理解：

```text
CBR / bps_target / bps_min / bps_max
H264 与 H265 码率差异
GOP 取舍
QP 当前先不主动调整
Header Mode
stride / heightStride 注意点
后续测试方向
```

当前策略仍保持克制：先区分 H264/H265 默认码率，不动 QP，不引入 VBR/AVBR，等 RTSP / IPC 场景跑起来后再结合真实码流继续调参。

#### 6. 测试素材

下载了两段公开测试素材到 `build/testdata` 用于本地验证：

```text
sample_city_1080p.mp4
big_buck_bunny_1080p_10s_10mb.mp4
sintel_720p_10s_5mb.mp4
```

其中 `sintel_720p_10s_5mb.mp4` 更适合作为当前动画测试片：

```text
1280x720
24fps
10s
240 frames
约 4.19 Mbps
```

这些文件位于 build 目录下，不纳入 git。

## 2026-08-31

### 摄像头 MJPEG 解码链路接入

今天把 USB 摄像头 MJPEG 采集接入到了现有摄像头基座里，保持对外仍然发布裸帧：

```text
V4L2 DQBUF MJPEG
  -> CamManager 投递到每路 DecodeWorker
  -> MPP JPEGD 解码
  -> 解码输出写入 DecodeWorker 私有 DmaBufferPool
  -> 发布 NV12 FramePacket 到 FrameHub
  -> DisplaySink / 其他 Sink 继续按裸帧处理
```

关键设计：

- `CameraSlot` 持有 `std::unique_ptr<DecodeWorker>`，每路 MJPEG 摄像头一个解码 worker。
- `DecodeWorker` 输入侧只保留最新一帧，避免解码排队造成延迟堆积。
- 被覆盖的 pending MJPEG packet 会自动释放 input lease，原始 V4L2 buffer 回到 return queue。
- 解码输出侧使用 `DmaBufferPool`，不是单帧 buffer，因为下游 sink 可能异步持有输出帧。
- output `FrameLease` 捕获 `std::shared_ptr<DmaBufferPool>` 和输出 `VideoFrame*`，最后一个 sink 释放后归还解码输出池。
- MJPEG 解码输出池按 MPP 外部 buffer 习惯预留：

```text
align16(width) * align16(height) * 2
```

这里没有按 NV12 理论 payload 的 `* 3 / 2` 申请，避免后续 MPP 对额外信息空间有要求时踩内存。

### 编解码公共类型整理

新增：

```text
include/hw/MppTypes.hpp
```

把 `MppCodec` 从 `MppDecoder.hpp` 中抽出来，供编码器和解码器共同使用：

```cpp
enum class MppCodec {
    MJPEG,
    H264,
    H265,
};
```

### 配置和显示日志增强

`CamManager::addCamera()` 现在会打印两段关键日志：

- 上层请求配置：设备、宽高、fps、像素格式、buffer 数量、dma heap。
- V4L2 驱动最终接受配置：实际宽高、实际 fps、实际像素格式、输入 buffer capacity。

如果是 MJPEG，还会额外打印解码输出池配置：

```text
format=NV12
outputBufferSize
outputBuffers
```

`DisplaySink` 的限流 fps 日志也补充了更多信息：

- 输入格式、输入宽高、输入 stride。
- 输入 `bytesUsed / capacity`。
- 输出 RGBA 宽高、输出 stride、输出 buffer index。
- 当前 sequence 和累计 dropped。

通用日志格式也收短为：

```text
HH:MM:SS.mmm [LEVEL] [Module:Line] function => message
```

去掉了线程 id，避免板端日志前缀过长。

### 板端 MJPEG Smoke Test

在 RK3568 板端使用临时 smoke 程序验证核心链路，不经过 Qt 显示：

```text
/dev/video10 MJPEG 640x480@30
  -> DecodeWorker
  -> NV12 640x480
  -> minimal Sink
```

结果：

```text
单路 /dev/video10：5s 收到 72 帧
单路 /dev/video12：5s 收到 142 帧
双路 /dev/video10 + /dev/video12：5s 分别收到约 85 / 89 帧
```

这说明：

- `CamManager -> DecodeWorker -> MPP MJPEG decode -> FrameHub -> Sink` 闭环已跑通。
- sink 收到的是解码后的 `NV12`，不是压缩 MJPEG。
- `/dev/video10` 和 `/dev/video12` 的实际输出节奏不同，后续做性能对比需要固定同一设备和同一配置。

### 当前待确认问题

板端 Qt demo 两路 MJPEG 显示时进程 CPU 约 `68%~72%`，但线程拆分后发现大头不完全在 MJPEG 解码：

- `QSGRenderThread` 经常 `20%~30%`。
- Mali 后台线程约 `8%~11%`。
- `mpp_dec_parser` 线程合计约几个到十来个点。
- `jpegd / rga / vop` 中断都按帧增长。

初步判断当前高 CPU 主要是完整显示链路成本：

```text
MJPEG -> JPEGD 解码为 NV12
      -> DisplaySink RGA 转 RGBA
      -> Qt SceneGraph 导入纹理并合成显示
```

下一步需要回到 MPP 接入前的版本，用单路 `/dev/video12`、YUV、`640x480`、QML 只保留一个裸 `MyItem` 的同工况做对照，拆清楚到底是 Qt 渲染、RGA、UVC copy 还是最近改动带来的额外开销。

### Qt 显示链路性能对照

为了拆清楚 CPU 占用来源，今天回到 MPP 接入前的历史提交做了两组板端对照。

#### 1. 老版裸 `MyItem`

测试点：

```text
commit: fda77b7
QML: 单个裸 MyItem
camera: /dev/video12
format: YUYV
size: 640x480
fps: 30
```

结果：

```text
DisplaySink fps: 约 30.5
appqt-demo CPU: 约 21%
RSS: 约 82MB
```

线程拆分大概为：

```text
QSGRenderThread: 5%~6%
DisplaySink/RGA worker: 4%~5%
Mali backend: 3%~4%
CamManager / Qt 主线程 / 其他线程: 剩余部分
```

中断观察：

```text
USB: 10s 约 2500 次
RGA: 10s 约 300 次
JPEGD: 不动
VOP/GPU: active
```

这个结果说明即使没有 MPP，单路 USB YUYV 预览也已经包含：

```text
UVC 收帧 / 内核拷贝
CamManager DQBUF/QBUF
DisplaySink RGA: YUYV -> RGBA
Qt SceneGraph: RGBA dma-buf -> texture -> 合成显示
```

#### 2. MPP 前的 NVR QML 布局

测试点：

```text
commit: f53f88b
QML: NVR 布局
camera: /dev/video12
format: YUYV
size: 640x480
fps: 30
```

结果：

```text
DisplaySink fps: 约 30.5
appqt-demo CPU: 约 23%~25%
RSS: 约 94MB
```

线程拆分大概为：

```text
QSGRenderThread: 7%~8%
DisplaySink/RGA worker: 4%~5%
Mali backend: 3%~4%
CamManager / Qt 主线程 / 其他线程: 剩余部分
```

结论：

- NVR QML 布局本身相比裸 `MyItem` 大约增加 `3%~4%` CPU。
- 主要增量在 `QSGRenderThread` 和额外 QML/Mali 线程。
- 这组数据没有 MPP 参与，因此不能把单路 YUYV 的 `20%+` 基础占用归因于 MPP。
- 后续如果最新版本单路 YUYV 达到约 `30%`，还需要继续拆当前版本相对 `f53f88b` 的新增开销，例如窗口尺寸、等比例 viewport、MouseArea、日志、事件队列和线程调度。

### DRM 直显链路验证

为了判断 Qt Quick 显示链路本身的消耗，今天把之前的 `drm_test` 整理进项目 `drm/` 目录，并新增一个最小摄像头直显 demo：

```text
drm/cam_drm_sink_demo.cpp
drm/wsl-build-cam-demo.sh
```

这个 demo 复用当前摄像头基座：

```text
CamManager
  -> FrameHub
  -> DrmSink
```

`DrmSink` 支持两条测试路径：

```text
YUYV 摄像头帧
  -> RGA 转 NV12
  -> DRM plane 直显

MJPEG 摄像头帧
  -> DecodeWorker / MPP JPEGD 解码成 NV12
  -> DRM plane 直显
```

板端测试命令示例：

```bash
cd /root/nfs/MultiCamRenderer/drm/build-wsl-aarch64
./cam_drm_sink_demo /dev/video12 20 yuyv
./cam_drm_sink_demo /dev/video12 20 mjpeg
```

#### 1. YUYV -> RGA NV12 -> DRM

测试配置：

```text
camera: /dev/video12
format: YUYV
size: 640x480
fps: 30
```

结果：

```text
DrmSink fps: 约 30.5
cam_drm_sink_demo CPU: 约 5%~6%
RSS: 约 3.5MB
```

线程观察：

```text
主线程基本空闲
DrmSink worker 约 5%~6%
```

#### 2. MJPEG -> MPP JPEGD NV12 -> DRM

测试配置：

```text
camera: /dev/video12
format: MJPEG
size: 640x480
fps: 30
```

结果：

```text
DrmSink fps: 约 30.5
cam_drm_sink_demo CPU: 约 13%~14%
RSS: 约 4.5MB
```

线程观察：

```text
mpp_dec_parser 约几个点
DecodeWorker / DrmSink 约 8% 左右
CamManager poll 线程约 2% 左右
```

中断观察：

```text
MJPEG 直显路径：JPEGD / VOP active，RGA 中断基本不增长
YUYV 直显路径：RGA / VOP active，JPEGD 不动
```

结论：

- 直接 DRM 预览明显比 Qt Quick 预览省 CPU 和内存。
- 当前 `CamManager + FrameHub + Sink + FrameLease` 基座没有暴露出明显性能瓶颈。
- Qt Quick 路径适合 UI、交互、布局和开发效率；DRM 路径适合后续高密度 NVR 预览。
- 后续可以保留两种显示后端：

```text
Qt Quick Sink: UI 友好，适合配置页、调试页、轻量预览。
DRM Sink: 性能优先，适合多路实时预览主画面。
```

## 2026-09-01

### MJPEG 解码参数语义收口

今天继续打磨 MJPEG 解码链路，重点把几个容易混淆的概念拆清楚：

```text
input capacity     : 输入 dma-buf 的总容量
input packet_size  : 当前 MJPEG 压缩帧的真实长度，也就是 V4L2 bytesused
output capacity    : 输出 dma-buf 的总容量
output layout      : 输出 NV12 的 width / height / stride / heightStride
```

底层 C 接口从一长串参数改成两个描述结构：

```c
RkMppInputPacket {
    fd,
    capacity,
    packet_size,
}

RkMppOutputFrame {
    fd,
    capacity,
    width,
    height,
    stride,
    height_stride,
}
```

这样 `mpp_buffer_import(input)` 使用 `input.capacity`，`mpp_packet_set_length()` 使用 `input.packet_size`，读代码时不会再把 buffer 容量和压缩包长度混在一起。

### MJPEG 输出 layout 由外层决定

由于当前 MJPEG 解码采用“调用方提供 output dma-buf”的模型，输出池和输出 layout 都由外层 `DecodeWorker` 决定：

```text
CamManager / DecodeWorker
  -> 根据 V4L2 最终接受的摄像头配置计算 NV12 output layout
  -> 按 MPP 外部解码输出规则申请 output DmaBufferPool
  -> 每帧 decode 前填写 output VideoFrame 的 layout
  -> MppDecoder 只负责把 input packet 解码到 output frame
```

因此 `MppDecoder::decodeMjpeg()` 不再解析 MJPEG 头，也不再自己猜输出宽高和 stride。底层 C 中的 JPEG 头解析工具也从 public header 中收回，只保留为内部工具，避免上层误用。

### VideoFrame 工具函数统一 buffer 计算

`include/VideoFrame.hpp` 新增一组工具函数，后续所有模块都优先使用这里，不再各自手算：

```cpp
videoFrameAlignUp()
videoFrameBytesPerPixelForStride()
videoFrameMinDimensionAlignment()
videoFrameAlignedStride()
videoFrameAlignedHeightStride()
videoFrameEffectiveStride()
videoFrameEffectiveHeightStride()
videoFrameBufferSizeFor()
videoFrameBufferSize()
videoFramePlaneOffset()
```

并明确区分两种 buffer size 模式：

```text
Payload:
  普通图像真实 payload，例如 NV12 = stride * heightStride * 3 / 2

MppDecoderOutput:
  RK MPP 外部解码输出预留，例如 NV12 = stride * heightStride * 2
```

已替换的调用点：

- `RgaEngine::bufferSizeFor()` / `requiredSize()`
- `MppDecoder::decodeMjpeg()`
- `CamManager::DecodeWorker` MJPEG 输出池大小
- Qt `DisplaySink` RGBA `bytesUsed`
- DRM sink NV12 `bytesUsed` 和 UV plane offset
- `MppEncoder` stride 判断
- `RgaCopySink` 测试池大小

### dma-buf sync 结论

这次重新确认了 `DMA_BUF_IOCTL_SYNC` 的语义：它主要用于 CPU mmap 访问 dma-buf 前后的 cache 同步，不是硬件设备之间的完成/可见性同步。

因此 MJPEG advanced 解码里：

```text
output fd:
  MPP 硬件写，后续 DRM/RGA 硬件读。
  不做 DMA_BUF_SYNC_WRITE。

input fd:
  当前 USB UVC MJPEG 多半是内核 CPU memcpy 写入 vb2 buffer。
  先暂时保留 READ sync，后续可以通过开关和 perf 数据判断是否继续裁掉。
```

### 当前优化思路

短期继续保持“简单但稳”的每帧导入模型：

```text
每帧 mpp_buffer_import(input)
每帧 mpp_packet_init_with_buffer()
每帧 task 完成后释放 packet / buffer 引用
```

理论上可以按 fd 或 bufferIndex 缓存 `MppBuffer/MppPacket`，减少每帧 import/init 开销，但这会引入更多生命周期问题：

- MPP task recycle 完成前不能复用 packet。
- 摄像头删除、重建、fd 复用时需要非常小心。
- 当前性能账里这块还不是主要瓶颈。

所以先把语义和边界打稳，等后续 perf 明确显示 `mpp_buffer_import()` / `mpp_packet_init_with_buffer()` 成为热点，再做缓存优化。

### 验证

本地交叉编译：

```bash
./wsl-build.sh
cd drm && ./wsl-build-cam-demo.sh
git diff --check
```

板端短测：

```bash
cd /root/nfs/MultiCamRenderer/drm/build-wsl-aarch64
./cam_drm_sink_demo /dev/video10 5 mjpeg
```

结果：

```text
MJPEG 640x480@30 -> DecodeWorker -> NV12 -> DRM
DrmSink fps 约 30
```

当前观察到 Qt Quick 单路 MJPEG 大约稳定在 `37%` CPU。按之前对照粗拆：

```text
Qt Quick 显示附加成本约 20% 左右
MJPEG 解码 + CamManager + RGA 等非 Qt 部分约 17% 左右
```

这个结果比早期两路 MJPEG Qt Quick `68%~72%` 的状态更健康，说明这轮 MJPEG 参数语义和同步路径收口是有价值的。

## 2026-09-06

### RTSP 拉流、解码与稳定裸帧输出

本日将已验证的 live555 RTSP 客户端接入 `Stream` 基座，形成一路网络视频的完整异步链路：

```text
live555 事件线程
  -> Annex-B 压缩 NALU 回调
  -> DecodeWorker 有界压缩队列（复制压缩数据）
  -> MPP H.264/H.265 解码
  -> RGA copy 到 Stream 自己的 DmaBufferPool
  -> readyQueue（稳定 FramePacket）
  -> StreamManager / FrameHub / Sink
```

新增模块：

- `RtspStream`：继承 `Stream`。不要求调用方传 codec，live555 从 SDP 识别 H.264/H.265 后，按实际 codec 初始化 MPP decoder。
- `Stream::DecodeWorker`：每路 `Stream` 独占一条 worker 线程，live555 回调只复制并投递压缩包，不在事件线程做 MPP/RGA 工作。
- `RtspStreamDemo`：最小拉流、MPP 解码、RGA 稳定化和 readyQueue 验证程序。

### 稳定输出与分辨率变化

MPP 解码输出是临时资源，不能直接交给外部。因此每帧都通过 RGA copy 到 `Stream` 自己的输出 `DmaBufferPool`，再以带 `FrameLease` 的 `FramePacket` 发布。当前每路输出池为 4 块 DMA buffer。

当码流中的输出 layout（宽、高、stride、heightStride、格式）发生变化时，创建新 pool 并切换到新 pool。旧 `FramePacket` 的 lease 会继续持有旧 pool，所以 Sink 尚未释放旧 dma-buf 时不会被提前回收。

### readyQueue 与丢帧策略

`RtspStream` 的 `readyQueueCapacity` 默认定为 `2`：它只缓存已经解码完成、但尚未被上游 Manager 取走的稳定裸帧。队列满时淘汰最旧帧，以控制实时显示延迟。录像不应依赖这条裸帧队列，后续应从压缩码流独立分支写文件。

同时增加基座 `LOG_WARN` 丢帧统计，按累计值、最多每秒输出一次，并在 stop 时输出最终累计值：

- `readyQueue淘汰`：新裸帧到来时淘汰尚未消费的最旧帧。
- `pool被下游lease占满`：所有 pool buffer 均被 Hub/Sink 持有，当前新裸帧无法落池。

当 pool 暂时没有空闲 buffer 时，先只淘汰一条最旧 ready 帧并重试 acquire；不会因为容量大于 1 而清空整个 readyQueue。

### 板端验证

在 RK3568 上拉取 RV1126B 的 `H.264 1280x720` RTSP 流，MPP/RGA 链路稳定约 `24 fps`。约 3370 帧解码输出中，readyQueue 容量为 2 时累计淘汰约 31 帧（约 0.9%），`pool被下游lease占满=0`。这是 demo 轮询与调度抖动造成的低延迟旧裸帧淘汰，不是 RTP/NALU 丢包，也不会破坏解码参考链。

`RtspStreamDemo` 直接调用 `stream.start()`，未经过 `StreamManager` 分配 id，因此日志中的 `streamId=-1` 属于预期现象。

### 构建

`drm/wsl-build-rtsp-mpp-demo.sh` 默认构建目标调整为：

```text
RtspStreamDemo.cpp
  -> build-wsl-aarch64/rtsp_stream_demo
```

仍可通过 `DEMO_SOURCE` 和 `DEMO_OUTPUT` 环境变量切换回旧的直接 MPP demo。

### Codec 类型收口、热路径异常规则与学习文档

为避免 RTSP、DRM demo 和后续其他输入源各自维护一份 `VideoCodec -> MppCodec` 的转换逻辑，新增通用 `include/VideoCodec.hpp`，并在 `include/hw/MppTypes.hpp` 集中提供：

```cpp
constexpr MppCodec toMppCodec(VideoCodec codec);
```

`RtspStream` 和 `rtsp_drm_sink_demo` 已删除本地重复的转换 `switch`，统一使用该函数。原 live555 目录中的 `VideoCodec.hh` 仅保留兼容 include，不再重复定义类型。

同时明确实时音视频模块的错误处理规则：

```text
禁止使用 throw / try / catch 作为错误控制流。
```

`AnnexBSink::afterGettingFrame()` 是每条 NALU 都会进入的热回调，现已移除 callback 外层 `try/catch`。正常路径下异常机制通常不会逐次栈展开，但异常不是实时链路合适的错误传递方式，且不应跨越 live555/C++ 回调边界。模块内部应使用 `bool`、空指针、`lastError()` 和项目 `LOG_WARN/LOG_ERROR` 就地报告错误。

新增文档：

```text
docs/rtsp_pull_flow.md
```

文档完整记录 `RtspStream::start()` 到 DESCRIBE / SETUP / PLAY、RTP 重组、Annex-B NALU、压缩队列、MPP、RGA、DMA pool、readyQueue 和停止清理顺序，并记录上述异常处理规则。

验证：

```bash
cd drm
./wsl-build-rtsp-mpp-demo.sh
./wsl-build-rtsp-demo.sh
git diff --check
```

两个 aarch64 RTSP 目标均交叉编译通过；仅保留已有 MPP C 源文件的 unused-function 编译警告。

## 2026-09-06

### 源码布局与 WSL 统一构建整理

为让模块职责和目录名一致，本次将实现源码收口为扁平的 `src/`，不再保留与具体实现无关的 `hw/`、`sink/` 和 `drm/live555/` 中间目录：

- MPP、RGA 与 rkmpp C 适配层移至 `src/`，对应公开头文件移至扁平的 `include/`。
- live555 拉流客户端 `Live555RtspClient`、`AnnexBSink` 直接置于 `src/`；third_party 的 live555 库仍只作为外部依赖保留在 `third_party/live555/`。
- `RgaCopySink` 和只依赖它的旧 `rga_test` 已移除；旧 `cam_manager_demo`、`test` 也已移除。
- 不依赖 DRM 的 RTSP 学习 demo 移至 `demo/`；`drm/` 只保留 DRM 显示相关 demo 与代码。

根目录 `wsl-build.sh` 现在是 RK3568 WSL 的唯一 demo 构建入口，统一生成：MPP、V4L2、DMA、FrameLease、RTSP、DRM，以及 Qt/RGA 的 `rga_ops_demo`。Qt 应用本身仍通过 `qt-demo/wsl-build.sh` 单独构建。

`demo/rga/build.sh` 改用 `qt-demo/wsl-toolchain.cmake` 和 `/opt/rk3568_kernel_pack` 的 WSL 交叉工具链；不再触碰非 WSL 的旧 toolchain 配置。

验证：

```bash
./wsl-build.sh
git diff --check
```

全部 aarch64 demo 目标和 `rga_ops_demo` 均交叉编译通过。

## 2026-09-06

### 统一 CMake 构建与 Qt 目录收口

工程改为由根目录 `CMakeLists.txt` 统一描述 RK3568 aarch64 构建目标。MPP、V4L2、RTSP、DRM 等非 Qt demo 和 Qt 目标不再各自维护一份手写编译命令。

WSL 下保留两个入口，且使用互不共享的 CMake build 目录：

```bash
./wsl-build.sh       # build/wsl-aarch64：全部非 Qt demo
./wsl-build-qt.sh    # build/wsl-aarch64-qt：Qt 应用与 rga_ops_demo
```

`wsl-build-qt.sh` 会先加载 Qt aarch64 环境，并将 `qt/app` 部署到 `qt/deploy-wsl-aarch64/bin/app`。非 Qt 脚本不加载 Qt 环境；两个配置缓存因此不会互相污染。

原 `qt-demo/` 目录重命名为 `qt/`，Qt 可执行 target 和部署文件从 `appqt-demo` 收口为 `app`。QML URI `QtDemo` 保持不变，它是模块标识，不是可执行程序名。

原 `demo/rga/build.sh`、`qt/wsl-build.sh` 和各自的 WSL CMake 入口已移除，避免出现第三套构建路径。非 WSL 的历史 toolchain 与构建脚本不在本次范围内。

验证：

```bash
./wsl-build.sh clean
./wsl-build-qt.sh clean
git diff --check
```

非 Qt 的 11 个 aarch64 目标、Qt `app` 与 `rga_ops_demo` 均交叉编译通过。

## 2026-09-08

### 完善 live555 客户端与 Annex-B Sink 学习文档

新增 `docs/live555_client_workflow.md`，将当前 RTSP 客户端的异步状态机按真实代码顺序整理为：

```text
start
→ DESCRIBE / SDP / MediaSession
→ MediaSubsessionIterator
→ initiate 本地 RTP 接收链
→ SETUP 传输协商
→ AnnexBSink::startPlaying
→ PLAY
→ RTP 重组后的 Annex-B NALU 回调
→ stop / EventTrigger / cleanup
```

文档额外说明了 `MediaSession` 与 `MediaSubsession` 的关系、未来音频 track 的接入方式、UDP/TCP 协商，以及配套 IPC 服务端 `reuseFirstSource=true` 时多客户端共享实时 source 的含义。

`AnnexBSink` 章节记录了可复用 `receiveBuffer_` 的 `00 00 00 01 + NALU payload` 内存布局、截断处理、静态 live555 回调转成员函数的原因，以及每次处理完成后必须调用 `continuePlaying()` 重新登记下一条 NALU 的原因。

同时将 `AnnexBSink.hh`、`Live555RtspClient.hh` 作为公开接口放入 `include/`，实现仍在 `src/`；相关回调和 SETUP 注释改为中文且补充准确的协议语义。

验证：

```bash
./wsl-build.sh build
git diff --check
```

三个 RTSP aarch64 demo 均重新编译、链接通过。

## 2026-09-08

### StreamManager 第一版、RTSP 状态机与压缩码流恢复

新增 `StreamManager`，用于管理多路 `Stream` 的生命周期，并将每路解码完成的稳定裸帧统一发布到其对应的 `FrameHub`。`CamManager` 保持独立；本模块只处理 RTSP 等非本地摄像头 source。

发布线程不使用 `poll()`：`Stream::enqueueDecodedFrame()` 在稳定 `FramePacket` 成功入每路 `readyQueue` 后触发 ready 回调，`StreamManager` 用 `condition_variable` 被动唤醒。`m_hasPendingFrames` 用作事件合并标志，因此多路同时到帧不会为每帧重复唤醒；发布线程醒来后先处理状态事件，再快照全部 `Streaming` stream，并排空各自当前的 readyQueue：

```text
RtspStream / DecodeWorker
  -> Stream readyQueue
  -> StreamManager publisher thread
  -> FrameHub
  -> Sink::onFrame(FramePacket)
```

`FrameHub` 对 Sink 只保存 `weak_ptr`，应用/Manager 必须持有 Sink 的 `shared_ptr` 生命周期。`FramePacket` 通过共享 `FrameLease` 保护 DMA buffer，最后一个消费者释放 packet 后 buffer 才归还所属 `DmaBufferPool`。因此 Sink 必须快速 move/排队/提交 packet 并返回，不能在 `onFrame()` 内做磁盘 I/O、同步算法或其他阻塞工作；当前所有 Stream 共用唯一发布线程。

新增独立 `demo/StreamManagerDemo.cpp` 与 `stream_manager_demo` CMake target，原有 RTSP demo 未改动。demo 默认拉取 `rtsp://192.168.1.5:8554/live`，可以通过第一个参数传入其他 URL，并打印每秒发布帧率及状态变化。

### 异步 RTSP 状态与重连世代

此前 `startStream()` 成功只表示 live555 事件线程已创建，不能代表服务端已接受播放。现在状态语义收口为：

```text
Ready / Stopped / Error
  -> Starting       live555 事件线程已启动，DESCRIBE/SETUP/PLAY 尚未完成
  -> Streaming      收到 PLAY 200 OK
  -> Stopping
  -> Stopped

DESCRIBE / SETUP / PLAY / RTP source 异步失败
  -> Error
```

`Live555RtspClient` 新增 `StateCallback`，由 `RtspStream` 转换为 `Stream::RuntimeState`。状态事件先进入 `StreamManager` 的待处理队列，再由发布线程更新 `StreamSlot`，避免 live555 回调线程直接争用 Manager 的 stream map 锁。

每次 start/stop 均递增 `runGeneration`。异步状态事件携带 generation；Manager 只接收与当前 slot 一致的事件，因此 stop/restart 后迟到的旧连接 `Stopped/Error` 不能覆盖新连接状态。新增 `getStreamState()` 供 UI/业务查询每路状态和最近异步错误。

### 压缩 NALU 队列溢出恢复

`Stream::DecodeWorker` 的压缩队列满时不再只丢单条 NALU 后继续向 MPP 发送 P/B 帧。新策略为：清空旧积压、进入等待恢复状态、丢弃普通 NALU，直到收到完整参数集和随机访问帧后才恢复：

```text
H264: SPS + PPS + IDR
H265: VPS + SPS + PPS + BLA / IDR / CRA
```

恢复点到达时，worker 在送参数集前执行 MPP `deinit -> init`，清理旧参考帧和内部积压，再将暂存参数集及随机访问帧送入解码器。当前压缩队列数量上限继续保持 `512`，未在本次擅自变更为时间阈值；后续应通过压测和实际 IDR 间隔再确定实时低延迟上限。

### 验证

```bash
./wsl-build.sh build
git diff --check
```

所有非 Qt aarch64 target 交叉编译通过。

在 RK3568（`192.168.1.4`）实测 `stream_manager_demo` 拉取 RV1126B（`192.168.1.5`）的 H.264 1280x720 流：`PLAY 200 OK` 后状态正确进入 `Streaming`，发布帧率稳定约 `23~25 fps`，正常退出会发送 TEARDOWN。使用未监听端口 `65534` 验证异步错误路径，状态正确进入 `Error`，错误信息为连接拒绝；不再错误显示为 `Streaming`。

压缩队列实际溢出后的“等待参数集 + IDR”恢复状态机已完成交叉编译和代码检查；尚未人为压慢 MPP 或构造压测码流触发该分支做板端运行验证。

## 2026-09-09

### 修正 IDR 恢复唤醒与补充链路学习注释

检查压缩 NALU 队列的恢复路径时发现：原逻辑在收齐参数集和随机访问帧后，会将恢复数据放入 `m_packetQueue`，但从 `enqueue()` 提前返回，未执行 `m_cv.notify_one()`。若 DecodeWorker 此时正睡在条件变量上，它会依赖下一条普通 NALU 到达后的通知才开始处理已入队的恢复数据。

现改为由 `enqueueRecoveryPacketLocked()` 返回“是否已真正入队可解码数据”：普通 NALU 丢弃、单独参数集暂存时返回 `false`；收齐“参数集 + IDR”并放入队列时返回 `true`。外层仅在正常入队或恢复边界完整入队时唤醒 DecodeWorker，恢复不再依赖下一条 NALU。

同时补充 `Stream`、`RtspStream`、`StreamManager` 内的中文学习注释，明确：

```text
队列溢出后旧参考链不可信
→ 参数集 + 随机访问帧已入队
→ DecodeWorker 重置 MPP
→ 重新 init 并解码恢复边界
```

`resetDroppedFrameStatistics()` 的统计字段说明移回函数内部，使 readyQueue 淘汰、pool lease 占满和 WARN 节流时间的语义与赋值位置对应。

验证：

```bash
./wsl-build.sh build
git diff --check
```

RTSP 相关 aarch64 target 交叉编译通过。

## 2026-09-09

### 收口全局 CompressedPacket 压缩码流类型

此前 H.264/H.265 的输入码流被临时装入 `VideoFrame` 再传给 `MppDecoder::sendPacket()`。这会混淆压缩数据与裸图像语义：压缩 NALU 不具备 width、height、stride、DMA 输出 buffer 或 `FrameLease` 等字段。

新增通用 `include/CompressedPacket.hpp`：

```cpp
struct CompressedPacket {
    VideoCodec codec;
    const uint8_t* data;
    size_t size;
    uint64_t timestampUs;
};
```

它是非 owning 的压缩码流 view，不依赖 MPP。调用方只在 `sendPacket()` 调用期间借用 `data`；跨线程队列仍由 `Stream::DecodeWorker::Packet` 内部的 `std::vector<uint8_t>` 持有数据。

数据边界收口为：

```text
CompressedPacket
  = H264/H265 压缩码流视图

VideoFrame
  = 解码后的裸图像 / DMA buffer 视图

FramePacket
  = VideoFrame + FrameLease
```

`Stream` 的网络输入、压缩队列项与 Annex-B 参数集/IDR 恢复状态机均改用通用 `VideoCodec`。仅在 DecodeWorker 实际初始化 MPP decoder 时才通过 `toMppCodec()` 转换为 MPP 私有类型；`MppDecoder::sendPacket()` 改为接受 `CompressedPacket` 并校验其 codec 与当前解码器一致。

同步更新了 `Stream`、RTSP MPP demo、DRM RTSP demo 和文件解码 demo 的所有 H264/H265 `sendPacket()` 调用。MJPEG 的 `decodeMjpeg(VideoFrame, VideoFrame)` 保持不变，因为它使用另一套 DMA fd 输入/输出接口。

验证：

```bash
./wsl-build.sh build
git diff --check
```

全部非 Qt aarch64 target 交叉编译通过。在 RK3568 上运行 `stream_manager_demo` 拉取 RV1126B H.264 1280x720 RTSP 流，状态正常进入 `Streaming`，发布帧率稳定约 `24 fps`，证明该类型重构未改变 RTSP → MPP → RGA → StreamManager 发布链路。

## 2026-09-09

### 收口 MPP 类型映射、VideoFrame 布局计算与 Qt 启动路径

将原先分别散落在 `MppDecoder.cpp`、`MppEncoder.cpp` 的公共 MPP 类型映射抽到：

```text
include/MppTypes.hpp   声明 MppCodec / 项目类型 ↔ MPP 类型的接口
src/MppTypes.cpp       唯一的 switch 映射实现
```

公共接口包括 `toMppCoding()`、`toMppFrameFormat()`、`fromMppFrameFormat()` 和
`mppCodecName()`。映射层可以表示 MJPEG/H264/H265 三种编码；实际能力仍由模块自身
校验，当前 `MppEncoder` 继续拒绝 MJPEG，`MppDecoder` 保持支持 MJPEG。

同时将 `VideoFrame.hpp` 中的布局辅助函数（stride/对齐、buffer size、plane offset）
迁入 `src/VideoFrame.cpp`。头文件只保留 `VideoFrame`、`FramePacket`、`FrameLease`、
枚举、接口声明及其语义文档，减少每个引用该头文件的编译单元重复编译实现。CMake 的
MPP 公共源集合和 Qt target 均加入 `MppTypes.cpp`、`VideoFrame.cpp`，避免漏链接。

板测 Qt demo 时发现两个原有启动问题，已修正：

- QML 实际资源路径为 `qrc:/qt/qml/QtDemo/Main.qml`，修复了 `main.cpp` 旧地址导致的
  `QQmlApplicationEngine failed to load component`；
- `qt/run.sh` 显式设置 `${QT6_DIR}/lib` 到 `LD_LIBRARY_PATH`，使 SSH/非交互 shell
  也能找到 `libQt6QuickControls2.so.6` 等 Qt 动态库。

### 过滤p帧
为了防止刚创建流时处于不太好的位置，比如会收到大量的p帧，这种数据包就不应该送解码器，所以创建流和数据包积压都统一走"等待恢复"
### 验证

```bash
./wsl-build.sh build
./wsl-build-qt.sh
git diff --check
```

所有非 Qt 和 Qt aarch64 target 均交叉编译通过。RK3568 上实测：

```text
/dev/video10 MJPEG
→ MPP MJPEG decode
→ RGA
→ Qt DisplaySink / EGLFS

640x480，约 30.26 fps，dropped=0
```

`stream_manager_demo` 的 H.264 RTSP 板测也保持约 24 fps；两次超时停止后均确认无
残留进程。

### 后续：推流服务端的客户端请求 IDR 控制

客户端已经在 `DecodeWorker` 中实现首帧/恢复门控：只有等到参数集加随机访问帧才将
压缩码流送入 MPP，避免新加入流时把无法独立解码的 P/B NALU 送给 decoder。

接入推流服务端与编码器 API 时，补充 RTSP 控制面请求关键帧：

```text
客户端：SETUP 200
  → SET_PARAMETER: request-key-frame: 1
  → PLAY

服务端：RTSPClientSession::handleCmd_SET_PARAMETER()
  → 解析 request-key-frame
  → 投递给编码线程调用编码器 API 请求 IDR
  → 编码器在 IDR 前输出 SPS/PPS（H265 还包括 VPS）
```

使用 live555 `RTSPClient::sendSetParameterCommand()` 和服务端
`RTSPClientSession::handleCmd_SET_PARAMETER()` 实现。该请求是低频控制数据；服务端对
同一编码器做约 300–500ms 的合并限流，多个客户端同时请求只实际触发一次 IDR。不能在
live555 事件线程直接执行可能阻塞的编码器调用，应投递给编码线程。

### RV1126B IPC 双路 RTSP 首版

新增 `ipc_app`：`/dev/video33`（VPSS scale0）作为 `/main`，输出
H.265 1920×1080@30；`/dev/video34`（VPSS scale1）作为 `/sub`，输出
H.264 1280×720@30。一个 `Live555RtspServer` 共用 8554 端口与一个 event-loop，
每路独立的 `RtspPublishSink` 在首个 PLAY 时启动本路 CamManager 与 MPP encoder，最后
一个客户端离开后停止本路采集与编码。

VPSS 节点拒绝 dma-heap 外部 buffer 导入时，`V4L2CameraSource` 自动回退到
`V4L2_MEMORY_MMAP + VIDIOC_EXPBUF`：采集 buffer 仍导出为 DMA fd，因此 VPSS 到 MPP
保持零拷贝。RV1126B 构建通过独立 toolchain/media_out 配置，并将运行时 RPATH 固定为
板端 `/oem/usr/lib`。

2026-09-11 板测：RV1126B 服务可正常启动；RK3568 分别及同时拉取 `/main`、`/sub`，
MPP 解码与 StreamManager 发布均稳定约 30fps。客户端 TEARDOWN 后对应的活动客户端数
回到 0，编码器正常 deinit；测试进程正常退出，8554 已释放。

### RV1126B：应用内 AIQ 生命周期与 OEM 开机自启

首次接入时，板端启动脚本中的独立 `aiq_v4l2_daemon` 与 `ipc_app` 分属两个进程：
daemon 持有 AIQ/3A，`ipc_app` 直接操作 VPSS V4L2 节点。应用退出后再次启动时，两个
生命周期不同步，容易留下 ISP 的 frame-lost/SOF disorder，并表现为第二次启动画面偏黑或
异常色彩。

现新增 `IspController`，由 `ipc_app` 成为唯一 ISP owner：

```text
启动：发现 sensor
  → preInit_scene(normal/day)
  → rk_aiq_uapi2_sysctl_init(IQ files)
  → prepare
  → start（AIQ 持续执行 AE/AWB/AF）
  → 创建并 STREAMON 两路 VPSS V4L2 Camera

退出：停止 RTSP/编码 worker
  → 两路 VPSS V4L2 STREAMOFF + poll 线程退出
  → AIQ stop
  → AIQ deinit
```

实现严格参考 SDK `project/app/aiq_v4l2_daemon/aiq_v4l2_daemon.c`：该程序就是“AIQ
owner + 其他用户程序直接打开 V4L2 节点”的模式，因此 IPC app 仅链接 `librkaiq.so`，不
引入 RKiPC 的 `librockit.so` / `RK_MPI_SYS_Init()` 管线。

`IpcApp` 不再在 RTSP 客户端 0↔1 时对 VPSS 执行 STREAMOFF/STREAMON；VPSS 持续采集，
无人观看时只由 `RtspPublishSink` 停止编码、丢弃裸帧。这样 3A 的 sensor 帧时钟保持连续。

OEM 部署约定：二进制放在 `/oem/usr/bin/multicam_ipc_app`，SDK 两份 `RkLunch.sh` 和
板端实际脚本在启动网络后后台启动它；`RkLunch-stop.sh` 向该进程发送 `SIGTERM`，让应用
按上述顺序清理。开机自启的 stdout/stderr 重定向至 `/dev/null`，因为 `/tmp` 为 tmpfs，
把 AIQ/MPP 标准输出长期重定向到文件会无上限消耗 RAM；需要排障时停止服务后手工前台运行。

验证：

```bash
./wsl-build-ipc.sh build
```

RV1126B 板测通过：在没有 `aiq_v4l2_daemon` 的条件下，应用可完成 AIQ
`init/prepare/start`，正常停止后无需重启板子即可再次启动并重新完成 AIQ 初始化；OEM 路径
下运行的 `multicam_ipc_app` 已成功注册 `rtsp://<board-ip>:8554/main`（H265）与 `/sub`
（H264）。
