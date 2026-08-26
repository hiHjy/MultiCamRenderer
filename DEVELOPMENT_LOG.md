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
