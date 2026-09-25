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

## 2026-09-12

### H265 RTSP/UDP 丢包：完整排查、根因与修复

#### 现象

RV1126B IPC 推送 `/main`（H265 1920×1080@30）时，手机播放器通常正常；但 RK3568 的
live555 拉流 → MPP 解码 → DRM 显示链路偶发绿屏/花屏，OBS/FFmpeg 日志还会出现：

```text
RTP: missed N packets
Could not find ref with POC ...
Error constructing the frame RPS
Skipping invalid undecodable NALU
```

H264 `/sub`（1280×720）稳定得多。MPP 侧一度持续报告：

```text
[RKMPP Decoder] 丢弃异常输出 errinfo=1 discard=0
```

静态画面比运动画面更容易暴露 MPP `errinfo=1`；但这不是“静止时 live555 或 MPP 偷懒”。静态
画面通常更容易压缩，不能只凭现象归因于“静态 IDR 更大、UDP 缓冲不足”。本次确认到 UDP socket
容量不足；静态/运动差异仍需以本地 Annex-B 回放进一步隔离。

随后以服务端编码输出日志按同一口径核对，实际测得本设备当前 MPP H265 RC 行为为：

```text
静态画面 IDR NALU：约 270–314 KiB
  272300 / 271753 / 269652 / 313388 / 314141 bytes

动态画面 IDR NALU：约 83–151 KiB
  151490 / 151343 / 106408 / 108052 / 101967 /
   97634 /  82975 /  90785 /  85921 /  86433 bytes
```

因此本次“静态时更易出错、运动时恢复”的直接差异是静态 IDR 实际大约多出 160–230 KiB，而不是
“静态编码必然更小”的通用规律。编码大小由具体的 MPP 码率控制、GOP 状态和图像内容共同决定，必须
以实际码流统计为准。

#### 不要先猜播放器或解码器：按数据边界分三段取证

为定位丢失发生的位置，临时加入了仅在 `MCR_PACKET_TRACE=1` 下启用的逐 NALU 日志，并分别
记录 H265 的时间戳、NAL 类型、长度：

```text
编码器输出（RtspPublishSink）
  → Live555RtspServer::pushAnnexBFrame()
  → AnnexBFrameQueue
  → AnnexBSource::deliverFrame()        # source_out，交给 live555 RTP sink 前
  → RTP/UDP 网络
  → Live555RtspClient / AnnexBSink
  → Stream::DecodeWorker::enqueue()     # client_enqueue
  → MPP decoder
```

同时新增临时 `rtsp_annexb_record_demo`，把客户端 `AnnexBSink` 已经重组完毕的 Annex-B NALU
直接落盘。它的意义是把“RTP 分片重组是否完整”与“MPP/DRM 是否显示正确”分开判断。

结论如下：

1. 编码器输出的 H265 NAL 类型、PTS、IDR 数量正常；每个 IDR 前存在 VPS/SPS/PPS，随后是
   IDR，普通帧为 type 1。
2. `AnnexBSource` 的 `source_out` 与编码器输出逐条对应，证明项目自己的
   `RtspPublishSink → AnnexBFrameQueue → AnnexBSource` 没有丢 NALU。
3. 默认 UDP socket 参数下，服务端已输出的一部分大 IDR 没有出现在客户端录制 Annex-B 文件中。
   该样本证明 `source_out` 之后（live555 RTP sink、内核 UDP 缓冲或网络）存在真实丢包；它只
   解释缺失 NALU，不能据此排除编码器或 MPP 对静态场景异常的影响。
4. `ffprobe -count_frames` 对增大缓存后的 60 秒 H265 录制文件统计为 `1800` 帧，正好对应
   `30 fps × 60 s`；该验证证明该次提高缓存后的 RTP→Annex-B 接收链路没有持续少帧，不能
   单独证明所有 MPP `errinfo` 都由网络导致。

临时诊断代码在定位完成后已经移除，避免实时链路长期保留字符串解析、日志 I/O 和额外 mutex。
保留 `rtsp_annexb_record_demo` 作为独立诊断工具：以后遇到播放器、解码器或网络争议时，可先
录制客户端收到的原始 Annex-B，再用 `ffprobe`/`ffplay`/MPP 分别验证。

#### 已证实根因之一：H265 UDP 突发超过默认 socket 队列余量

默认内核值为：

```text
net.core.rmem_default = 212992
net.core.rmem_max     = 212992
net.core.wmem_default = 212992
net.core.wmem_max     = 212992
```

Linux 对 `SO_RCVBUF`/`SO_SNDBUF` 有内部记账和倍增语义，实际可观察到的 socket 缓冲约为
`425984` 字节量级。1080p H265 的单张 IDR 常为数百 KiB，live555 会依据 MTU 将其分为数百个
RFC 7798 FU RTP 包。这里必须理解：

```text
H265 NALU 被 RTP FU 分片
≠ 每个 FU 会被发送端按帧率均匀节流
≠ UDP 有反压、重传或可靠传输
```

一张 IDR 的大量 UDP datagram 会在很短时间内连续写入 socket；每包除 payload 外还有 skb/协议
记账开销。默认约 416 KiB 的收发队列无法可靠容纳该突发时，内核会丢 UDP datagram。IDR 只要
丢失一个 FU 分片，整个 NALU 就不能重组/解码；随后的 P 帧继续引用失去的参考图像，MPP 会输出
`errinfo=1`，FFmpeg 则报告 POC/RPS 缺参考。这是已由逐 NALU 对照确认的网络故障模式，但不是
“静态画面 MPP 异常”的完整归因。

本设备实测的静态约 300 KiB IDR 需要约两百多个 RTP UDP 包突发发送；动态约 100 KiB IDR 仅需
约几十个包。前者显著更容易越过默认 socket 队列余量，这与“静态阶段丢 IDR、动态阶段未复现”的
现象直接一致。

运动时“看起来恢复”可能与完整 IDR、码率控制、参考结构或 MPP 行为有关；当前数据不足以把它
归因于其中任意一项。`fDurationInMicroseconds` 影响帧间调度，却不能为一张 IDR 内部的数百个
UDP FU 提供可靠性；它不是 UDP 丢包的替代修复。

#### 修复

代码层保持显式 socket 扩容，避免依赖某一发行版的微小默认值：

```text
服务端 VideoSubsession::createNewRTPSink()
  → increaseSendBufferTo(..., 4 MiB)

客户端 Live555RtspClient::setupNextSubsession()
  → increaseReceiveBufferTo(..., 4 MiB)
```

系统层把 socket 默认值和上限设为：

```text
RV1126B RTSP 服务端：
  net.core.wmem_default = 4 MiB
  net.core.wmem_max     = 16 MiB

RK3568 RTSP 客户端：
  net.core.rmem_default = 4 MiB
  net.core.rmem_max     = 16 MiB
```

4 MiB 是默认突发余量；16 MiB 是未来 4K/更大 IDR 时可申请的上限，不会在启动时为每一个 socket
预先分配 16 MiB。当前代码请求 4 MiB，Linux 的 socket 缓冲倍增语义下可获得约 8 MiB 级实际
容量；未来以真实最大 IDR、码率和客户端数压测后，再决定是否将代码请求目标提高到 8 MiB。

持久化方式不需要重编 kernel、更不需要向未知分区 `dd`：运行时 `sysctl -w` 已经生效，说明内核
本来就支持该参数。只需在 rootfs 开机阶段配置：

```text
RK3568：/etc/sysctl.d/90-multicam-rtp.conf
  由现有 /etc/init.d/S02sysctl 自动加载

RV1126B：/etc/init.d/S19multicam-rtp-sysctl
  在 S21appinit 启动 IPC 应用前设置 wmem 参数
```

两块板子重启后均已回读确认配置生效。临时把实际 socket 缓冲提升到 8 MiB 后，H265 UDP 连续
60 秒录制得到完整 1800 帧，证明该次 UDP 传输问题消失；仍需将同一次落盘 Annex-B 直接喂给
MPP，作为静态 `errinfo` 的最终归因测试。

#### MPP 异常输出的正确处理

`src/mpp_simple.c` 保留并收紧了异常帧逻辑：当 `mpp_frame_get_errinfo(frame) != 0` 或
`mpp_frame_get_discard(frame) != 0` 时，打印一次完整的 format、尺寸、stride、fd、PTS 诊断并
直接释放该 MPP 输出，不再交给 RGA/DRM。

```text
错误/应丢弃的 MPP NV12
  → 不做 RGA copy
  → 不进入 Display/FrameHub
  → 防止不完整 buffer 被显示成瞬时绿屏或花屏
```

这不是“吞掉网络故障”，而是下游的安全边界；网络恢复仍依赖下一组完整的参数集 + IDR。该底层
错误过滤与打印属于长期保留的防御代码，不随临时 RTP/NAL 调试一同删除。

#### AnnexBSource 的时序原则

`AnnexBSource` 每次被 RTP sink 请求时直接取出一个 NALU，`fDurationInMicroseconds` 固定为零。
IPC 的摄像头采集和编码器本身已按真实帧率生产数据；在这里额外用下一条 NALU 的 PTS 推导时长，
既没有必要，还会遇到 MPP 独立 VPS/SPS/PPS header 的 `PTS=0` 与真实帧时间戳相减这一错误边界。
因此不保留 look-ahead 节流逻辑。

### live555 依赖统一

项目此前存在“头文件和静态库可能来自不同 live555 版本”的风险。现统一为 upstream
`live.2021.05.03`（`v2021.05.03-tree`），该版本与 RK3568 板端原有
`libliveMedia.so.94` 对应。

```text
third_party/live555/include/             # 同一源码原样导出的头文件
third_party/live555/lib/aarch64-rk3568/  # RK3568 Buildroot toolchain 静态库
third_party/live555/lib/aarch64-rv1126b/ # RV1126B SDK toolchain 静态库
```

`CMakeLists.txt` 新增 `MCR_LIVE555_TARGET`，`wsl-build.sh` 固定选择 `rk3568`，
`wsl-build-ipc.sh` 固定选择 `rv1126b`。这保证同一次 target 构建不会再把一套头文件和另一套
工具链构建的静态库混用。旧的单一 `lib/aarch64/` 已不再使用。

旧 live555 的 `TaskScheduler::doEventLoop()` 退出标志是 `char volatile*`，而新版本是
`EventLoopWatchVariable`；`Live555RtspServer` 按 `LIVEMEDIA_LIBRARY_VERSION_INT` 做了编译期
兼容分支，避免切回 2021.05.03 后出现类型不匹配。

### MPP SDK 库更新

两套 SDK 使用的 MPP 源码/根文件系统库统一更新到 Rockchip upstream `develop` 的：

```text
0986d01294d5c2449c14cf13af9b740368c33967
2026-08-26  fix[vproc]: Fix calloc transposed args
SONAME: librockchip_mpp.so.1
```

- RK3568 SDK 原 `external/mpp` 为 2023-09-15 版本。已切换至上述提交，并保留
  `mcr-before-mpp-20260912` 分支作为源码回退点；已编好的新版 `librockchip_mpp.so.0` 和
  `librockchip_vpu.so.0` 已直接放入当前 Buildroot target rootfs 的 `usr/lib/`。
- RV1126B SDK 原 MPP 源码快照为 2026-02-13。旧快照保存为
  `media/mpp/mpp-rv1126b-d7dd00e4-backup-20260912`，新版按 RV1126B SoC、其自身 aarch64
  工具链重新编译成功，并替换当前 OEM rootfs 的 `oem/usr/lib/` 中 MPP/VPU 库。

两边的 `.so` 都已核对 AArch64 架构与 `librockchip_mpp.so.1` SONAME；RV1126B rootfs 中的
库与本次交叉编译产物 SHA256 一致。

注意：本次是将已验证产物直接同步至“当前 rootfs 目录”，没有重新打完整固件镜像、更没有刷写
kernel 或分区。下次进行 SDK 全量构建/打包后，应再核对最终 image 中的 MPP 版本字符串。

### 其他本次改动

- `rtsp_drm_sink_demo` 默认 URL 改为 `/sub`，运行方式改为持续运行；启动前统一屏蔽
  `SIGINT`/`SIGTERM`，主线程 `sigwait()` 收到 Ctrl+C 后按 RTSP → MPP → DRM 正常 stop，便于
  长时间观察显示链路，不再由 demo 固定超时自行退出。
- `StreamManagerDemo` 默认 URL 改为 IPC 的 `/main`。
- 临时 NAL 序列统计、编码前/服务端 source/client enqueue 三段 trace、解码帧率统计和 socket
  实际值打印已全部删除；保留独立录制 demo 与长期需要的 MPP 错误帧过滤。

### 本次验证

```bash
./wsl-build.sh
git diff --check
```

RK3568 非 Qt aarch64 demos 交叉构建通过，其中包含：RTSP 拉流、MPP 解码、DRM 显示、
Stream/StreamManager、Annex-B 录制与 RTSP server 相关 target。

## 2026-09-13

### 新 RTSP 客户端加入时请求下一帧 IDR

`Live555RtspServer` 原有的 `onClientActiveChanged(bool)` 只表达某一路客户端人数的
`0 → 1` 和 `1 → 0` 边界：前者启动编码器，后者停止编码器。首个客户端启动新编码器时，编码序列
天然从 IDR 开始；但第二个及后续客户端在已有编码序列中加入，只能等待下一次周期 IDR，最坏等待
一个 GOP。

为此在 `Live555RtspServer::StreamConfig` 增加 `onAdditionalClientStarted`：仅当一个新客户端
完成 PLAY，且该路原本已存在播放客户端时触发。IPC App 将此回调接到
`RtspPublishSink::requestKeyFrame()`。

```text
第一个客户端 PLAY：0 → 1
  → onClientActiveChanged(true)
  → PublishSink init MPP
  → 第一帧天然 IDR

后续客户端 PLAY：1 → 2 / 2 → 3
  → onAdditionalClientStarted()
  → PublishSink 仅置位 m_keyFrameRequested
  → 编码 worker 下一次 sendFrame() 前调用 MPP_ENC_SET_IDR_FRAME
  → 下一帧输出 VPS/SPS/PPS + IDR
```

live555 回调运行在事件线程，不能直接调用 MPP。`RtspPublishSink` 将重复请求合并为一个 bool；只有
其编码 worker 串行调用 `MppEncoder::requestKeyFrame()`，因此不存在跨线程同时操作 MPP 的风险。若
强制 IDR 命令失败，仅记录 warning，正常编码与周期 IDR 仍继续。

板测：在 RV1126B 上先启动一个 RK3568 `/main` 客户端，再加入第二、第三个客户端；服务端分别记录
`active clients=2/3`，随后 PublishSink 均成功记录“已请求 MPP 下一帧 IDR”。两路 RK3568 客户端在
整个测试过程中稳定发布约 29–31 fps。

已将验证过的 RV1126B `ipc_app` 安装到 `/oem/usr/bin/multicam_ipc_app`，原程序备份为：

```text
/oem/usr/bin/multicam_ipc_app.20260913-idr-request.bak
```

### CamManager 单路故障隔离与 V4L2 自动恢复

此前 `CamManager::pollOnce()` 遇到任一路 camera 的 `POLLERR/POLLHUP/POLLNVAL`、`DQBUF`
或回收 `QBUF` 失败时会返回失败；`run()` 因而可能退出整个 poll 线程。现在将故障收敛为
**单路状态机**，其他 camera 的采集不受影响。

```text
单路 V4L2 异常
  → CameraSlot.state = Error，记录错误与 errorGeneration
  → 从下一轮 poll fd 快照中排除该路
  → 投递 ProcessError 到 CamManager poll 线程
  → STREAMOFF，等待全部旧 FrameLease 归还
  → 按 1 / 2 / 5 / 10 秒退避投递 RestartCamera
  → 轻恢复：复用 fd/buffer，重新 QBUF + STREAMON
  → 完整恢复：close → open → configure → DMA buffer setup → STREAMON
  → 成功回到 Streaming；失败继续退避，不退出 poll 线程
```

恢复中的所有 `DQBUF/QBUF/STREAMOFF/STREAMON/open/close` 仍只由 poll 线程执行。外部线程、
FrameLease 析构回调和故障检测都只投递命令或归还事件，避免跨线程操作同一个 V4L2 fd。

每个输出的原始 camera buffer 都有 `leaseGeneration` 与 `inFlightFrames`：重启前必须等旧 lease
全部归还，绝不为了恢复而强制释放下游仍在读取的 DMA buffer。等待超过两秒只记录 warning；恢复
成功后 generation 自增，旧的 buffer index 不会被错误 QBUF 到新一代驱动队列。

`POLLHUP/POLLNVAL` 直接请求完整重建；纯 `POLLERR` 或 DQBUF 错误先尝试轻恢复，连续失败后升级
完整重建。设备尚未重新枚举时，`openDevice()` 失败会继续按最大 10 秒间隔重试。

新增公开接口：

```cpp
CamManager::requestCameraRecovery(cameraId,
                                  CamManager::RecoveryMode::RestartStream,
                                  reason);
```

供已经明确知道上游 VPSS/V4L2 管线需要重置的调用方使用；接口只标记状态并投递命令，不在调用线程
直接操作硬件。

### 恢复验证 demo

新增 `cam_recovery_stress_demo`。默认模式在真实 V4L2 设备上依次验证：轻恢复、完整重建、持有
FrameLease 时不安全重启的阻止，以及 stop/start 跨代 lease 防护。

```bash
/tmp/cam_recovery_stress_demo /dev/video10 yuyv 640 480 30 1
```

RK3568 `/dev/video10` UVC 摄像头实测一轮通过，四项测试全部恢复出帧；持有旧 lease 超过两秒时
如预期只告警，归还后才恢复。此前连续五轮模拟压力测试也全部通过。

新增 `physical` 模式用于真实 USB 拔插，不注入软件故障、不设置恢复超时：

```bash
/tmp/cam_recovery_stress_demo physical /dev/video10 yuyv 640 480 30
```

该模式每两秒打印一次采集 fps。拔出后等待 CamManager 自动恢复，插回后重新出现帧率即表示恢复
完成，`Ctrl+C` 正常退出。当前测试使用固定设备路径；若 USB 重插后内核改分配为其他 `/dev/videoX`，
需要使用稳定的 udev symlink 或另行实现按 USB 身份重新发现，不能盲目自动选择任意 video 节点。

### RTSP 压缩包恢复缓存收敛

`Stream` 在压缩 NALU 队列溢出后等待“参数集 + IDR”恢复。此前等待期间参数集可能持续累计；现在
仅保留当前 access unit 的参数集：H264 为 SPS/PPS，H265 为 VPS/SPS/PPS。参数集 timestamp 切换时
丢弃上一组尚未等到 IDR 的残留数据，避免无界增长或混用两组编码参数。

### 本次验证

```bash
./wsl-build.sh
git diff --check
```

RK3568 非 Qt aarch64 demo 全量交叉构建通过；`cam_recovery_stress_demo` 已部署到 RK3568 并完成
模拟恢复与真实 USB 拔插恢复验证。

## 2026-09-14

### RV1126B IPC 编码前 OSD：FreeType + RGA + MPP + RTSP

本次将 OSD 作为 **IPC 推流链中 MPP 编码之前的一步** 接入，而不是作为 RTSP 客户端或
播放器的叠加效果。这样所有 RTSP 客户端、录像文件和后续外网转发看到的是同一份已叠字码流。

最终主路径：

```text
VPSS NV12 FramePacket
  -> RtspPublishSink 编码 worker
  -> RGA copy 到本路私有 NV12 DMA-BUF
  -> OsdRenderer 合成小 RGBA 时间文字层
  -> RGA 在最终 NV12 上直接画 AI 检测框（AI 接入后使用）
  -> MPP H264/H265 编码
  -> Live555RtspServer
```

`RtspPublishSink` 仍然只保留“最新待编码帧”这一条队列。OSD 输出不需要再建裸帧队列或
`DmaBufferPool`：每路只有一个编码 worker，`MppEncoder::sendFrame()` 同步返回后输入 DMA-BUF
已经不再被 MPP 使用，因此一块私有 NV12 输出 DMA-BUF 可以安全循环复用。

#### 新增 `core/osd`

- `FreeTypeTextRenderer`：将单行 UTF-8 文本栅格化为只在 CPU 内存存在的 `TextBitmap`。
  第一版支持常用中英文/数字、基础 kerning；复杂文字连写若未来需要，再在字形排版层接入 HarfBuzz。
- `OsdRenderer`：初版为单张小 RGBA 文字层；随后在 2026-09-15 重构为多个独立文字对象，并按纵向位置
  合并为若干全宽小 RGBA 条带（详见该日日志）。CPU 重绘只发生在文字内容或样式变化时，不会每帧重绘
  全屏 overlay。
- `OsdRenderer::setRectangles()` 只保存 `std::vector<RgaRect>`，不为框单独申请 DMA-BUF。
  `composite()` 在最终 NV12 上通过 RGA 直接画框；AI 模块未来只负责产出检测框坐标。

第一版使用普通（非预乘）alpha RGBA 文字层：黑色半透明背景为 `RGB=0,A=160`，白字为
`RGB=255,A=FreeType 覆盖率`；RGA 使用 `IM_ALPHA_BLEND_DST_OVER` 在合成时按 alpha 混合。

#### `RgaEngine` 扩展

`RgaOperation` 新增 overlay 裁剪/目标矩形和统一框样式字段；`RgaEngine` 新增：

```cpp
bool convertColor(const VideoFrame& src, VideoFrame& dst);
bool composite(const VideoFrame& source,
               const VideoFrame& overlay,
               VideoFrame& destination,
               const RgaOperation& op);
bool drawRectangles(VideoFrame& destination,
                    const std::vector<RgaRect>& rectangles,
                    const RgaOperation& op);
```

小 overlay 的实际 RGA 步骤不是创建全屏 RGBA 图，而是：先把完整 NV12 视频 copy 到私有输出，随后只对
`768x96` 文字区域做三输入合成。`source` 和 `destination` 强制使用不同 DMA-BUF，避免原地读写的硬件
语义不明确。矩形框直接写 destination。

新增 `freetype_bitmap_demo`，使用 `img/1.png` 完整验证：RGBA 输入 → NV12 → 小 RGBA 时间文字合成
→ NV12 绿色矩形框 → RGBA PNG 输出。RV1126B 板测结果正确。

#### IPC 接入与字体部署

`IpcApp` 的 `/main`（H265 1920x1080）和 `/sub`（H264 1280x720）都启用 OSD，文字为：

```text
main  YYYY-MM-DD HH:MM:SS
sub   YYYY-MM-DD HH:MM:SS
```

当前两路均配置 `768x96` 的小 RGBA overlay，放置在 `(24, 24)`；双缓冲合计约 `576 KiB/路`。

字体不是源码资产，运行期从以下路径读取：

```text
/oem/usr/share/fonts/DejaVuSans.ttf
```

已在当前 RV1126B 板子创建该目录并部署测试字体。后续打 SDK/根文件系统时，需将目标字体同步到同一路径；
若 OSD 未来显示中文，应替换为覆盖中文字符的 TTF/OTF。OSD 初始化失败会明确报错并停止该路编码器，
不会静默推送未叠字画面。

#### RV1126B 构建与 clangd

新增 `wsl-build-rv1126b.sh`，通过同一份根 CMakeLists 构建 `ipc_app` 和 `freetype_bitmap_demo`。
FreeType 头文件置于 `third_party/freetype/include/`，动态库继续使用 RV1126B SDK `media_out` 中的
`libfreetype.so`，不把 SDK 绝对路径散到源码中。

`tools/update_compile_commands.sh` 现在会合并 `build/rv1126b-aarch64/compile_commands.json`。此前根目录
数据库漏掉 RV1126B 的 IPC target，clangd 找不到 `OsdRenderer.hpp`，并连带误报
`shared_ptr<RtspPublishSink>` 不能转换为 `shared_ptr<Sink>`；现在根数据库已有 IPC 的完整 include/
sysroot 参数。

#### 本次验证

```bash
./wsl-build-rv1126b.sh
./wsl-build.sh
git diff --check
```

- RV1126B `ipc_app` 和 `freetype_bitmap_demo` 均交叉构建通过。
- RK3568 非 Qt demos 全量交叉构建通过。
- RV1126B 上临时启动新 `ipc_app`，分别拉取 `/main`、`/sub`：两路日志均确认 OSD 初始化、私有 NV12
  DMA-BUF 申请、RGA 启动成功。
- 从 `/main` RTSP 码流实际抓取 PNG，确认 `main 2026-09-14 ...` 已位于编码后的画面中。
- 板子测试完成后已恢复旧自启程序；本提交后的部署将单独替换 `/oem/usr/bin/multicam_ipc_app`。

### CamManager 恢复逻辑可读性收口

上一版自动恢复已经具备正确的线程边界和压力测试，但恢复状态字段、eventfd 命名与无效唤醒混在一起，
阅读时容易把“正确性保护”与“历史遗留/日志辅助”混为一谈。本次不扩展媒体能力，只收口可读性并删除
确认无效的状态。

`CameraRuntimeStatus` 现在按职责补齐中文注释，阅读时只需按四组理解：

```text
控制命令：m_commandQueue
FrameLease 归还：m_returnQueue + inFlightFrames
故障恢复：restartAttempts + nextRestartTime + requiresDeviceReopen
拒绝旧事件：recoveryGeneration + leaseGeneration
```

本次命名调整：

```text
errorGeneration       → recoveryGeneration
  表示第几轮故障恢复，用于拒绝旧的 ProcessError/RestartCamera 命令。

requiresFullRecovery  → requiresDeviceReopen
  true 时执行 close → open → configure → DMA buffer setup，而非仅 STREAMON。

restartNotBefore      → nextRestartTime
  表示本轮最早允许下一次恢复尝试的时间。

m_returnEventFd       → m_pollWakeEventFd
notifyReturnEvent()   → wakePollThread()
drainReturnEvent()    → drainPollWakeEvent()
```

最后一组改名尤其重要：该 eventfd 不只在 FrameLease 归还时使用，外部 `postCommand()`、停机请求也会
使用它把阻塞在 Linux `poll()` 中的 poll 线程唤醒。旧名字会误导阅读者以为它只与 return queue 有关。

删除 `lastRevents`：审查确认该字段只被写入和清零，从未参与恢复判断、日志或对外状态，因此属于死状态。

同时删除 `executeCommand()` 内部多处 poll 线程唤醒自身的 `m_camCv.notify_one()` 与 eventfd 写入。
真正需要保留唤醒的地方只有外部线程投递命令、归还 FrameLease、请求停止时：

```text
无 camera fd：poll 线程在 condition_variable 上等待，m_camCv 唤醒它
有 camera fd：poll 线程阻塞在 Linux poll()，m_pollWakeEventFd 唤醒它
```

两种唤醒机制并非重复：condition_variable 不能中断 Linux `poll()`，eventfd 也不能替代“没有任何 fd 时”的
condition_variable 等待。

### pollOnce 控制顺序

恢复检查调整为易读的三步：

```text
1. drainCommands()
   执行上一轮或外部线程已经投递的 Start/Stop/Delete/ProcessError/Restart 命令。

2. drainReturnedFrames()
   处理下游归还的 FrameLease，QBUF 并减少 inFlightFrames。

3. checkAndPostCameraRestartCommands()
   检查旧 FrameLease 是否已全部归还、退避时间是否到期；满足时只投递恢复命令。
```

第 3 步投递的新命令不在当前位置立即执行，而是经 eventfd 唤醒后进入下一次命令处理阶段执行。这样避免在
每一轮 `pollOnce()` 重复扫描所有相机两次；V4L2 的 DQBUF/QBUF/STREAMON/STREAMOFF 仍只在 poll 线程串行执行。

首次恢复退避调整为 `0 / 1 / 2 / 5 / 10` 秒：首轮允许立即尝试恢复；连续失败后再逐步退避，避免设备确实
离线时反复 open/configure/STREAMON 刷 CPU 和日志。

### 后续防止“状态债”演变为屎山的约束

后续不允许遇到边缘问题就直接新增 bool、generation 或 notify。每次修改 CamManager/Stream 状态机时必须先回答：

```text
1. 这个字段保护什么具体错误？谁写、谁读？
2. 现有字段是否已能表达该状态？能否用明确状态枚举替代 bool 组合？
3. 这个通知实际唤醒哪一个等待点？没有等待者时是否只是无效通知？
4. 新状态是否有压力 demo 或最小复现验证？
5. 命名是否让不熟悉实现的人能直接读出业务语义？
```

计划按“小步、可验证”推进：先维持现有摄像头恢复压力 demo；后续若继续扩展恢复逻辑，优先将恢复字段
收进独立 `RecoveryState`，再考虑新增状态，而非继续把字段平铺在 `CameraRuntimeStatus`。全局 `m_lastError`
可能被其他 camera 的成功操作清空、`delCamera()` 与 `shutdownPolling()` 并发边界需要进一步收口，这两项记录为
后续可靠性任务，本次不借可读性整理扩大修改范围。

### 本次验证

```bash
./wsl-build.sh
./wsl-build-ipc.sh
git diff --check
```

RK3568 非 Qt aarch64 demos 与 RV1126B `ipc_app` 均交叉构建通过。

## 2026-09-15

### MPP 编码输入 layout 的职责收口

此前 `IpcApp::makePublishConfig()` 同时填写了编码策略和输入图像 layout：

```text
codec / fps / bitrate / GOP
+ width / height / stride / heightStride / inputFormat
```

后半部分不应由最上层 IPC 应用猜测。特别是 `stride` 取决于 VPSS/V4L2 的实际协商和硬件对齐，
例如 1920 宽图像在某些来源上可能使用 2304 stride。项目统一遵守的原则仍是：

```text
谁生产真实图像，谁填写真实 layout；
谁最靠近首帧，谁把 layout 交给硬件模块。
```

本次职责划分调整为：

```text
IpcApp
  → 只填写 codec / fps / bitratePreset / gop（编码策略）

RtspPublishSink
  → 收到本轮第一张真实 VPSS VideoFrame
  → 复制策略配置到 activeEncoderConfig
  → 填写 width / height / stride / heightStride / inputFormat
  → 调用 MppEncoder::init(activeEncoderConfig)

MppEncoder
  → 将完整 config 转为 MPP prep:* 配置
  → 后续 sendFrame() 校验所有帧的 layout 不变后再送 DMA fd + timestamp
```

`MppEncoderConfig` 保留 layout 字段是必要的：编码器不像解码器能从 H264/H265 码流参数集获取
图像布局，DMA fd 本身也不携带宽高、stride、NV12/RGBA 等元信息。MPP 在首次
`encode_put_frame()` 前必须已经收到：

```text
prep:width / prep:height / prep:hor_stride / prep:ver_stride / prep:format
```

但这些字段不再由 `IpcApp` 对外配置；`RtspPublishSink` 每次 active 编码周期都生成新的局部
`activeEncoderConfig`，因此不会把上一次会话的 layout 残留到下一次。

### 本次验证

```bash
./wsl-build.sh
./wsl-build-rv1126b.sh
```

两套交叉构建均通过。RV1126B `192.168.1.6` 使用临时 `ipc_app` 分别拉取 `/main` 与 `/sub`：

```text
main H265: 1920x1080, stride=1920x1080, bitrate=5054400, gop=30
sub  H264: 1280x720,  stride=1280x720, bitrate=3456000, gop=30
```

日志确认每路均先由首帧补齐 MPP prep 配置，随后 MPP 初始化及 ffmpeg UDP 拉流成功。
测试结束后已停止临时程序并恢复板端 `/oem/usr/bin/multicam_ipc_app` 旧自启版本；本次提交不覆盖
板端正式二进制。

### OSD 文字对象、条带合成与检测框层级

OSD 外部接口使用多个 `OsdTextObject`。每个对象以唯一 `id` 识别；`anchor=Absolute` 时使用相对视频可见区域
左上角的绝对 `left/top`，`TopRight` 等边缘锚点则使用 `edgeOffsetX/Y`。边缘锚点的最终坐标由
`OsdRenderer` 在 `composite()` 获取实际视频和文字 bitmap 尺寸后计算，所以上层不必预先知道分辨率。
`paddingX/paddingY` 是文字到自身半透明背景边缘的间隔。`RgbaColor` 使用普通直观的
`{red, green, blue, alpha}`；FreeType 的 coverage 仅乘到 alpha，RGB 不预乘，RGA 以
`IM_ALPHA_BLEND_DST_OVER` 做普通 alpha 混合。

为兼容 RV1126B 当前 RGA 驱动的 RGBA 非零横向 crop 问题，条带的 x 固定为 0、宽固定为整张视频宽；但高度
只覆盖文字所在的最小纵向范围。首次 `composite()`（或对象的 `top`、纵向 padding、字号变化后）会按对象的
纵向区间自动分组：重叠或相距不超过 16 像素的对象共用一条带，相隔很远的对象各自一条。文字内容/颜色等普通
更新只将所属条带标脏并重绘该条带，不重新分组、不重新申请 DMA-BUF。

每帧路径为：

```text
VPSS NV12 --RGA copy--> 私有 NV12 输出
每个 RGBA 文字条带 --RGA composite--> 该输出的对应纵向区域
AI 矩形列表 --RGA drawRectangles--> 同一输出
MPP 编码
```

因此文字和检测框重叠时，检测框永远在最上层：框线会压在文字及其背景上。这是当前确定且可预期的图层规则。
`RgaEngine` 不调用 `imcheck`；封装已在提交 RGA 前完成自身的 frame、矩形、stride 与容量校验，随后直接提交
RGA 操作，避免额外 SDK 校验落入热路径。

### 本次验证

```bash
./wsl-build-rv1126b.sh
./wsl-build.sh
git diff --check
```

- 两套交叉构建通过。
- RV1126B `192.168.1.6` 上运行 `freetype_bitmap_demo`，输入 `img/1.png`（1090x783）后成功生成
  `osd_time.png`。
- 输出图确认：左上黄色 `CAM-01` 和白色时间属于同一顶部条带；中部 `DETECTION` 属于另一条带；绿色检测框
  的上边线故意横穿 `DETECTION`，验证“框最后画、覆盖文字”的规则。

### OSD 热路径复核

本次在提交前额外检查了条带更新的性能与状态正确性，并修正两项问题：

- 时间字符串更新不再重复 `FT_New_Face()` 打开字体。只有 `textPixelHeight` 变化才重新打开字体；普通文字变化
  仅调用 `renderText()` 重建 CPU `TextBitmap`。
- 文字长度、`left` 或 `paddingX` 变化时，不再错误沿用旧的对象矩形。该对象会在所属条带下次重绘前重新计算横向
  placement；只有 `top`、纵向 padding、字号或 bitmap 高度变化才重新分组。

常态 IPC OSD 的成本为：每帧一次 NV12 底图 copy、每个纵向条带一次 RGA composite、最后一次 RGA 框绘制。
CPU 写 RGBA 与 DMA cache sync 只发生在文字更新时（当前时钟为每秒一次），没有每帧字体加载、DMA-BUF 申请或
对象分组。若未来将很多文字放到相距很远的纵向位置，会相应增加条带和 RGA composite 次数；顶部/底部等少量
条带是推荐布局。

再次在 RV1126B 运行 demo，并模拟将时间更新为更长的 `"... UTC"` 文本，确认同条带内对象重算 placement 后
没有裁剪或错位；`imcheck` 已从项目 RGA 封装的所有热路径中完全移除。

随后补充 `OsdAnchor`：除 `Absolute` 的 `left/top` 外，`TopRight` 等锚点使用 `edgeOffsetX/Y`。这使
`RtspPublishSink` 创建对象时不需要预先取得 VPSS 帧尺寸；`OsdRenderer` 在首次合成时按真实视频宽高和当前
文字宽度计算最终位置。RV1126B demo 已用右上角的加长 `"... UTC"` 时间文本验证，右边保持 24 像素间距。

### OSD 按分辨率自适应缩放

OSD 配置统一以 `OsdRendererConfig::designWidth/designHeight` 表示设计分辨率，IPC 当前明确设为
`1920x1080`。`OsdTextObject` 的字号、绝对坐标、锚点边距和 padding 都是设计像素；首次
`composite()` 得到实际 VPSS `VideoFrame` 后，`OsdRenderer` 取：

```text
scale = min(videoWidth / designWidth, videoHeight / designHeight)
```

并换算成真实像素。这样 1080p 保留 48px 字和 24px 边距，1280x720 自动使用约 32px 字和 16px 边距。
分辨率运行中变化时，已有的 layout 重建路径会重新打开相应字号的 FreeType face、重建 bitmap 和条带；正常
30fps 路径只做一次比例/字号比较，不会反复加载字体。

AI 检测框的 `RgaRect` 坐标保持“当前视频帧真实坐标”，不由 OSD 二次缩放；只有其 `lineWidth` 作为视觉样式
按同一比例缩放。条带对象的合并阈值也按比例缩放，确保 720p 与 1080p 保持相同布局语义。

RV1126B `192.168.1.6` 实测通过：

- 输入 `1280x720`：文字、背景、右上锚点边距和框线均为 1080p 的约 2/3，输出正常。
- 输入 `1920x1080`：维持设计尺寸，输出正常。
- 两次均经过 PNG RGBA -> RGA NV12 -> OSD -> RGA RGBA 的真实硬件路径；检测框仍按既定规则覆盖文字。

## 2026-09-16

### RTSP 播放事件收口到 PublishSink

此前 `IpcApp` 在每个 `StreamConfig` 中配置两段 lambda：一段处理首/末客户端的编码器启停，
另一段处理后来客户端的 IDR 请求。这使应用层知道了本应属于推流模块的固定策略，且 main/sub
重复配置同一份行为。

本次将 `StreamConfig` 收回为纯 RTSP 描述：

```text
streamName + codec
```

`Live555RtspServer::addStream()` 直接接收该路 `shared_ptr<RtspPublishSink>`，但 Server 内部仅保存
`weak_ptr<RtspPublishSink>`；实际 Sink 仍由 `IpcApp` 持有，避免 Server/Sink 形成循环引用。
Server 在 live555 线程维护每路已 PLAY 客户端数，并只将三种有业务意义的边界事件交给 Sink：

```text
0 -> 1       FirstClientStarted
N -> N + 1   AdditionalClientStarted
1 -> 0       LastClientStopped
```

`RtspPublishSink::onClientPlaybackEvent()` 统一处理这些策略：首客户端 `setActive(true)`，后来客户端
合并一次 `requestKeyFrame()`，最后客户端 `setActive(false)`。事件处理只修改 worker 控制标志；OSD、
MPP 控制和编码仍只在 Sink 的 worker 线程串行执行。`Live555RtspServer::cleanupLive555()` 对仍有活动
客户端的流补发 `LastClientStopped`，所以应用退出路径不再需要手工逐路 `setActive(false)`。

最初为隔离编译依赖引入过通用 Observer 接口；复核后发现当前项目唯一的接收者就是
`RtspPublishSink`，这层抽象增加了阅读成本而没有实际收益，因此没有保留。Server 明确依赖自己的
发布 Sink 更符合当前 IPC 推流服务的职责。相应地，`mcr_rtsp_server` 明确加入 MPP/RGA 的头文件搜索路径，
最终媒体动态库仍由 `ipc_app` 统一链接。

同时按应用代码可读性要求，`IpcApp.cpp` 中的相机、编码、RTSP 三类配置均直接写在 `main()`，删除了
仅供这一处调用的 `makeCameraConfig()`、`makePublishConfig()`、`makeRtspStreamConfig()` 工厂函数。

### RV1126B 实机回归

在 RV1126B `192.168.1.6` 上，先停止原 `/oem/usr/bin/multicam_ipc_app`，确认 8554 已释放，再将本次
构建的 `build/rv1126b-aarch64/ipc_app` 复制为 `/tmp/multicam_ipc_app_chain_test` 单独启动。测试顺序为：

```text
1. ffmpeg UDP 拉 rtsp://192.168.1.6:8554/main
2. 2 秒后追加第二个 /main 客户端
3. 同时拉 rtsp://192.168.1.6:8554/sub
4. 三路各持续 8 秒，输出到 null muxer
```

结果：三个 ffmpeg 均退出码 0；首个 `/main` 收到 241 帧、追加 `/main` 收到 240 帧、`/sub` 收到 246 帧，
均约 30fps。板端日志确认：

```text
首个 main/sub 客户端 -> FirstClientStarted -> 编码器按首张真实 VPSS 帧初始化
追加 main 客户端     -> AdditionalClientStarted -> worker 请求 MPP 下一帧 IDR
最后 main 客户端离开 -> LastClientStopped -> 清队列、MPP deinit
程序收到 SIGTERM     -> 剩余 sub 流 LastClientStopped -> MPP deinit
```

追加 H265 `/main` 客户端的启动窗口中，ffmpeg 曾报告少量 `PPS id out of range` 与一次
`Could not find ref with POC 28`，随后持续正常出帧并跑满测试时长。原因是新客户端接入共享实时源到
请求 IDR 真正输出之间，仍可能先收到极少量无参考边界的 P/B 帧；本次的 IDR 请求已使其快速恢复，
但这不应被记录为“新客户端零解码告警”。后续若产品要求新客户端绝对无启动告警，需要进一步研究
live555 对新 RTP sink 的首包门控，而不是在应用层重复请求 IDR。

测试结束后已优雅停止并删除 `/tmp` 临时二进制，重新启动 `/oem/usr/bin/multicam_ipc_app`；最终确认板端
仅保留一个正式应用进程并监听 TCP 8554，本次提交不覆盖 `/oem` 正式程序。

### 本次验证

```bash
./wsl-build-rv1126b.sh
./wsl-build.sh
git diff --check
```

RV1126B `ipc_app` 与 RK3568 非 Qt 目标均交叉构建通过。

## 2026-09-19

### 音频模块已知问题汇总

音频模块（`src/core/audio/` 与 `demo/audio_*`）在 `feature/audio-core` 上完成了 WebRTC APM 2.1
（AGC2）接入和两块板子的实机调音，可用的配置结论已经写在 `docs/audio_astats_and_apm_notes.md`。
这里只集中记录**目前仍未解决**的问题，避免后面重复排查。

> 这几个问题同时收口在项目根目录的 `KNOWN_ISSUES.md`，并会随修复情况更新；本节是当天排查过程的
> 快照，两者不一致时以 `KNOWN_ISSUES.md` 为准。

#### 1. RV1126B 播放通路只支持双声道（驱动层限制，应用层不迁就）

现象：RV1126B 上任何**单声道**播放都是静音，双声道正常。板端实测：

```text
aplay -D plughw:0,0 -v generated.wav                  -> channels : 1    静音
aplay -D plughw:0,0 -v generated-long-48k-stereo.wav  -> channels : 2    有声
speaker-test -D plughw:0,0 -c 1 -t sine -f 1000 -l 1  -> 静音
speaker-test -D plughw:0,0 -c 2 -t sine -f 1000 -l 1  -> 有声
```

同一个 `generated.wav` 在 RK3568 上播放正常，所以不是文件本身或 ALSA 工具的问题。

根因定位到 SDK 驱动（`sysdrv/source/kernel/sound/soc/`）：

| 文件 | 角色 | 声道声明 |
| --- | --- | --- |
| `codecs/rk3506_codec.c` | 麦克风 ADC（`audio_codec` / `audio_codec_pmu`） | **只有 `.capture` 1~2ch，没有 `.playback`** |
| `codecs/rk_dsm.c` | 喇叭功放（`acdcdig_dsm`） | `.playback` **2~2** |
| `rockchip/rockchip_sai.c` | CPU DAI（`sai2`，TDM 控制器） | `.playback` 1~512 |

这条 `multicodecs` 链路上唯一能出声的 DAI 是 DSM 功放，而它物理上就是双声道时隙。

但 **DSM 的 2 声道约束没有传到 ALSA 层**。板端 `hw:0,0` 报的是：

```text
CHANNELS: [1 512]
```

512 来自 `rockchip_sai.c:1407`（SAI 的 TDM 能力），ASoC 没能把它和 DSM 的 2~2 求成交集。
后果是 ALSA 的 plug 层认为单声道属于「设备原生支持」，**不做 upmix**，单声道样本被原样送进驱动。
而 `rk_dsm_hw_params()` 从头到尾没有读过 `params_channels()`，它只按采样率配时钟、按格式配位宽，
I2S 接收固定按双时隙配置 —— 单声道数据进了双时隙帧，结果是静音。

**结论与决定：** 这是驱动/机器驱动的缺陷，不是应用该绕的坑。**不在 `AudioPlayback` 里加
「一律请求双声道」这类迁就逻辑**，`audio_playback_config_init()` 保持 1 声道默认值，
`audio_playback_open_auto()` 依旧按调用方请求的声道数协商。后续要么等 Rockchip 修（让 DSM 的
声道约束正确暴露，或在 `rk_dsm_hw_params()` 里按 `params_channels()` 配置），要么在板级 ALSA
配置（`asound.conf` 的 plug 层）里显式约束，而不是改应用。

> 曾经在 `AudioPlayback.c` 里试过「优先请求双声道」，已回退。

#### 2. RK3568 播放 XRUN 风暴（未定位）

`audio_opus_playback_demo` 在 RK3568 上播放 4.94 秒的 Opus 文件：

```text
247 个包 / 246 次 XRUN / 829ms 跑完        （正常应约 4940ms）
```

XRUN 是 ALSA 的缓冲区跑穿。播放方向是 underrun：DMA 把缓冲里的数据读完了，应用还没送来新的；
采集方向是 overrun：应用取得太慢，新数据覆盖旧数据。两个方向都返回 `-EPIPE`（`-32`）。

写入循环里的调试输出呈**严格奇偶交替**：

```text
write#0 ret=960  state=RUNNING
write#1 ret=-32  state=XRUN
write#2 ret=960  state=RUNNING
write#3 ret=-32  state=XRUN
```

之所以是「跑完」而不是「卡住」，是因为现有代码在 `-EPIPE` 分支里调 `snd_pcm_prepare()` 把缓冲区
整个清空后立刻重试，缓冲永远填不满，`snd_pcm_writei` 也就永远不阻塞。

已排除的因素：同一块板、同一组 ALSA 参数（period 960 / buffer 3840）下，最小测试程序 `alsapb`
走 `plughw:1,0` 是 **0 XRUN / 5000.2ms**，走 `default` 是 2 XRUN / 4970ms。设备和参数都没问题，
问题出在 demo 的播放路径里，但具体是哪一处还没查到就暂停了。

**待办：** 用同一份 Opus 文件跑 `alsapb` 复现，再逐步收敛到 `AudioPlayback` / `AudioDecoder`
的具体分支。

#### 3. RV1126B 的 codec 控件会被关流和上电流程重置

每次采集/播放结束后，ACodec 的数字增益和开关会被复位，实测掉 **17dB**：

```text
Digital Gain       -> 0     (-95dB)
PGA Gain           -> 16    （原本 31）
ADC Switch         -> off
DAC Digital Volume -> 0     （静音）
```

板端脚本（`/root/audio-test/apm-listen.sh`）每轮开录前都重设一遍。这不是麦克风或硬件问题，
早期「麦克风偏置没打开」的结论已经被推翻。

#### 4. 底噪问题不能靠调增益解决

板端实测：`fixedDigitalGainDb = 30` 时各项指标都变好（峰值 -6.3 dBFS、零削顶），但再往上到 40
就过载削顶。关键在于**补电平的同时底噪同步放大**，听感是「响但糊」。

最终采用**不加增益 + 降噪 High + 高通**，听感明显优于原始采集。详见
`docs/audio_astats_and_apm_notes.md`。

#### 5. `Power Amplifier` 控件会让 amixer / alsactl 直接 abort

DSM 的 `Power Amplifier` 是 `SOC_ENUM_EXT`，读 TLV 时返回 `EINVAL`，`amixer` 和 `alsactl` 会直接
中止。为此单独写了 `ctldump`（只做 `SNDRV_CTL_IOCTL_ELEM_*` 的裸调用）供板端列控件和读写。
另外这个开关是 DAPM 托管的，写进去会立刻被拉回 `off`，只在播放期间为 `on`。

#### 附：一个还没解释的现象

`speaker-test` 打印的 `Time per period` 和理论周期时长对不上：单声道 period 4096 帧在 48kHz 下
应该是 85.3ms，实测报 **1.408s**；双声道 period 2048 帧应该是 42.7ms，实测报 **5.888s**。两个数都
远大于理论值，但双声道确实听得到正常声音。这可能是 `speaker-test` 自身的计时口径问题，也可能是
驱动侧时钟或周期上报有偏差，**尚未查证**，先记在这里。

### 本次验证

试改「优先请求双声道」时交叉构建通过；按要求回退后重新构建，仍然通过：

```bash
git checkout -- src/core/audio/AudioPlayback.c          # 回退迁就逻辑
cmake --build build/rv1126b-aarch64 \
      --target audio_opus_playback_demo audio_capture_apm_pcm_demo
```

`git status` 现在只剩 `include/core/audio/AudioCapture.h`（注释补充）和未跟踪的
`docs/audio_astats_and_apm_notes.md`。

### RV1126B 单声道播放无声：定位与 ALSA 规避（2026-09-19）

这一问题的现象很明确：RV1126B 可以正常采集 48 kHz 单声道 PCM，录出的
`test_c1.wav` 在 Windows 上播放也有声音；但板端直接播放同一份单声道 WAV 没有声音。把内容转成
双声道后，板端立即可以正常播放。因此问题不在 Opus、APM、录音数据或应用层 PCM 队列，而在板端的
ALSA 硬件播放通道约束。

复现命令：

```bash
arecord -D default -f S16_LE -c 1 -r 48000 -d 5 -t wav test_c1.wav
aplay test_c1.wav                     # 原始系统：无声
```

#### 1. 驱动侧事实

SDK 中实际有相关源码；不是没有 RV1126B 音频驱动源码：

```text
sysdrv/source/kernel/sound/soc/codecs/rk_dsm.c
sysdrv/source/kernel/sound/soc/codecs/rk3506_codec.c
sysdrv/source/kernel/sound/soc/rockchip/rockchip_sai.c
sysdrv/source/kernel/sound/soc/rockchip/rockchip_multicodecs.c
```

板端声卡是 `rockchiprv1126b`，machine driver 是 `rk-multicodecs`。DSM codec 的 playback
DAI 明确声明为 `channels_min = 2`、`channels_max = 2`，即模拟输出硬件本质上是双声道；但
machine driver / SAI 的运行时能力把通道范围暴露得过宽，原生单声道可以一路走到硬件路径，结果没有
正确复制到左右声道，表现为静音。采集通道正常，不代表播放单声道也正确。

`rk_dsm_hw_params()` 只配置采样率、采样格式和固定双 slot，并未按 `params_channels()` 建立正确的
播放通道约束；`rk-multicodecs` 也没有补这一约束。这是 BSP 驱动能力描述与实际硬件能力不一致的问题。

可选的内核长期修复是只对 playback 的 `.startup` 增加 ALSA channels min/max = 2 约束，让原生
`hw:0,0` 直接拒绝 mono；但该修复本身不会把 mono 自动扩成 stereo，仍需要 ALSA plug 或应用做路由。
它需要重编内核和刷固件，当前不作为恢复声音的前提。

#### 2. 最终采用：板级 ALSA plug 自动 mono -> stereo

不修改通用 `AudioPlayback`，也不要求所有调用者伪造双声道。应用仍可以按真实音频内容请求 mono；板级
`default` PCM 使用 ALSA `plug` 在输出端把 mono 复制到左右两个硬件通道，stereo 则保持双声道原样输出。

配置已放入 RV1126B SDK：

```text
/home/hjy/2026-07-18/rv1126b_linux_ipc_xiaoyu/project/app/etc/asound.conf
```

内容如下：

```conf
pcm.!default {
    type plug
    slave {
        pcm "hw:0,0"
        channels 2
    }
    route_policy duplicate
}
```

选择 `/etc/asound.conf`，而不是改 `/usr/share/alsa/alsa.conf`：主 ALSA 配置会自动 include
`/etc/asound.conf`，SDK 的 `project/app/etc/` 又会随 rootfs 打包到目标机 `/etc/`。当前运行板也已安装
同一份配置；后续构建整体固件时会自然带上，无需再手工处理。

#### 3. 验证结果

在带该配置的环境下执行 `aplay -v test_c1.wav`，ALSA 明确打印路由表：

```text
Transformation table:
0 <- 0
1 <- 0
```

即单声道输入的第 0 路被复制到硬件左、右声道；实听恢复正常。双声道素材走 2ch 硬件路径也已验证有声。
这证明它是可撤销、低风险的板级兼容层处理，不是把问题掩盖在 Opus 或应用代码中。

#### 4. 与 APM 调试的边界

此前两天还同时进行了 APM 2.1（AGC2）与底噪调试。当前策略是不启用自适应数字增益，保留高等级降噪和
高通；原因是低信噪比下单纯拉高增益会同时放大底噪，不能解决听感问题。该工作与本节的 mono 无声是两个
独立问题：前者影响录音响度和噪声，后者是播放端通道路由错误。详细参数与 astats 数据见
`docs/audio_astats_and_apm_notes.md`。

#### 5. 后续事项

- `KNOWN_ISSUES.md` 的 RV1126B 单声道 playback 条目仍保留为 BSP 驱动缺陷；状态应理解为“驱动未修、
  板级 ALSA 配置已完成可靠规避”。
- 若后续维护 BSP 内核，可给 `rk-multicodecs` playback 加双声道硬约束，避免原生 `hw:0,0` 被错误地当成
  支持单声道；届时仍保留 `default` plug，保证通用应用的 mono 兼容性。
- codec 控件在设备 power transition 后会重置（数字增益、PGA、ADC/DAC 开关），这与本问题独立，采集测试
  脚本仍需在每轮开始前重设必要控件。

### 音频核心链路、AEC3 与 PlaybackManager 落地（2026-09-19）

本次把之前归档中的 ALSA/Opus 原型收敛为项目内的 `core/audio` 基座。原则是：C 层保留可独立板测的
设备与编解码能力；以后 RTSP、WebRTC 与 App 只通过薄 C++ 封装使用，不把 ALSA 或 WebRTC C++ 类型泄露到
业务层。

#### 1. 上下行职责收敛

上行管理器原名 `AudioInputManager`，已更名为 `AudioCaptureManager`，避免 input 语义不清。它的常态数据流为：

```text
AudioCapture (ALSA, 48kHz/mono/S16_LE, 10ms)
    -> [可选] AudioApm
    -> AudioEncoder（当前 Opus，后续可扩展 G.711/AAC）
    -> AudioEncodedPacket callback
```

下行新增 `AudioPlaybackManager`，它是业务/网络层唯一入口；`AudioPlayback` 仍只是同步写 ALSA 的底层工具，
不对外承担线程或队列职责：

```text
有序编码音频包
    -> AudioPlaybackManager::enqueue_packet()
    -> 有界编码包队列
    -> 唯一播放线程
    -> AudioDecoder（当前 Opus）
    -> 10ms PCM reference + AudioPlayback/ALSA
```

`AudioPlaybackManager` 不处理 RTP 乱序、PLC 或 jitter buffer；这些属于将来的 RTSP/WebRTC 通话接收层。
它默认只留 8 个 20ms Opus 包，满时淘汰最旧包以控制实时延迟。

#### 2. ALSA 启动 XRUN 与解决

首次板测使用 10ms period、40ms ALSA 环形缓冲时出现连续 XRUN。原因不是 Opus 解码丢包：声卡在收到第一段
10ms PCM 后立即按硬件时钟消耗样本；如果后续 `snd_pcm_writei()` 稍晚，缓冲见底即发生 underrun (`-EPIPE`)。
声卡不会等待下一包，因为等待会停止硬件播放时钟。

最终 PlaybackManager 采用两层本地保护：

```text
ALSA hardware buffer: 8 x 10ms = 80ms
首次出声前预填：  3 x 20ms Opus = 约 60ms
```

这只是声卡启动与调度余量，不是网络 jitter buffer，也不做网络包重排。RV1126B 实测同一份 5.88 秒录音：

```text
入队 294 包 / 解码 294 包 / 播放 282240 PCM frames
队列淘汰 0 / ALSA 写失败 0 / XRUN 0
```

新增 `demo/AudioPlaybackManagerDemo.c`，会按 packet 文件中的相对 PTS 节奏入队，能测试真实的播放线程与
短队列；旧 `audio_opus_playback_demo` 保留为直接解码播放诊断工具。

#### 3. WebRTC APM 与 AEC3 的运行期边界

`AudioApm` 保持 C ABI，但内部使用 webrtc-audio-processing 2.1。总开关为：

```c
AudioApmConfig::enableAudioProcessing
```

默认值改为 **0**。监控 RTSP 音频的目标是保留环境声，默认不应使用语音降噪、高通或增益：

```text
RTSP 监控默认：采集 -> 原始 PCM -> Opus -> RTSP
```

通话开始时调用：

```c
audio_capture_manager_request_echo_cancellation(&captureManager, 1);
```

采集线程会在下一个 10ms 安全边界重建为 `AEC3 + 高通 + 降噪`；播放线程不直接操作 APM，而是把即将写 ALSA
的每个 10ms PCM 复制给 `AudioCaptureManager` 的 reference queue。通话结束传 `0`，恢复为原始 PCM 直通。
真正通话接入前仍需依据播放链路实测填写 `aecStreamDelayMs`；当前 `0` 仅表示尚未标定，不代表真实声学延迟。

当 APM 关闭时，采集线程不创建 `webrtc::AudioProcessing`，也不调用 bypass 处理；PCM 直接进入预热/编码路径。
当 APM/AEC 开启时，预热期的 PCM 虽然不编码输出，仍要送入 APM，以便降噪和回声消除器完成内部状态收敛。

#### 4. 采集启动预热

`AudioCaptureManagerConfig::warmupDiscardDurationMs` 默认 500ms。每次采集启动，或 APM/AEC 模式重建后：

```text
前 500ms：不送编码器、不推网络
APM 关闭：直接丢原始 PCM
APM 开启：送 APM 内部收敛，但丢其输出
500ms 后：开始正常编码
```

这样可以屏蔽 ALSA/模拟麦克风启动瞬态与 APM 冷启动音色跳变，而不会把首批突兀音频推上 RTSP。RV1126B 2 秒
短录实测仅输出 69 个 20ms Opus 包，符合约 0.5 秒预热后再开始输出的预期。

#### 5. A/B 听感与验证工具

新增 `demo/AudioApmAbTest.sh`。它使用 `AudioCaptureApmPcmDemo` 在**同一次**采集中分别保存原始 PCM 与 APM
处理后 PCM，再按“原始 -> APM”顺序 `aplay`。刻意不经过 Opus，避免编码器成为 A/B 比较变量：

```bash
sh demo/AudioApmAbTest.sh 12
```

APM/AEC 专用 demo（`audio_capture_apm_pcm_demo`、`audio_apm_gain_demo`、`audio_apm_aec_smoke_demo`）均显式打开
`enableAudioProcessing`，不会因产品默认关闭 APM 而退化为原始 PCM。

#### 6. 本次构建与板测

- `./wsl-build.sh`：通过。
- `./wsl-build-rv1126b.sh`：通过。
- RV1126B `audio_apm_aec_smoke_demo`：AEC3 reverse/capture 连续 200 个 10ms block 成功。
- `git diff --check`：通过。

构建仍会显示 webrtc-audio-processing 上游头文件中的 unused-parameter warning；它来自第三方头文件，不影响本次
目标代码构建，暂不在项目内 patch 上游库。

#### 7. 后续计划

1. 先逐段 review 当前 C 音频全链路；
2. 在 C core 之上做薄 C++ RAII 封装（无异常，显式错误与 callback），让 RTSP/App 不直接调用 C 函数；
3. 再接 RTSP 音频收发；jitter buffer、RTP 时序、PLC 留在协议/通话层实现；
4. 最后用真实扬声器/麦克风链路标定 AEC stream delay 与听感。

### C++ AudioPipeline / PlaybackPipeline 重构与实时内存打磨（2026-09-21）

本次不接 RTSP、不接 WebRTC 协议层，先把项目内音频的“媒体图”收敛成能独立复用、可板测的
C++ 基座。此前的 `AudioCaptureManager` / `AudioPlaybackManager` C 实现可以完成板测，但把采集、
APM、Opus、队列、播放/AEC reference 固定串成一条链，不能表达未来实际需要的多支路：例如监控
RTSP 需要原始 PCM，通话支路需要 AEC/降噪后的 PCM；二者不应因为通话建立而互相重启或中断。

重构完成后核查确认：旧 C manager 已没有 App、RTSP 或 IPC 调用点，只剩各自的旧 demo。因此本次直接删除
`AudioCaptureManager` / `AudioPlaybackManager` 及其 demo，而不是让两套音频架构长期并存。保留的 C 层只有
可独立复用的同步媒体工具，所有新的业务链路只从 C++ pipeline 进入。

#### 1. 分层原则与新上行图

底层 C 文件只保留同步、单职责媒体工具：

```text
AudioCapture      : ALSA -> PCM callback
AudioApm          : PCM -> PCM
AudioEncoder      : PCM -> EncodedAudioPacket
AudioDecoder      : EncodedAudioPacket -> PCM
AudioPlayback     : PCM -> ALSA
```

它们不理解 RTSP、WebRTC、Hub 或业务线程。`AudioPipeline` 是 App 显式拥有的 C++ 上行入口，
不是全局单例：App 在生命周期内 `startCapture()` / `stopCapture()`，未来 RTSP、录音或通话模块按需
保存订阅句柄。

```text
C AudioCapture（唯一采集线程，干净 PCM）
    -> rawPcmHub
       ├-> raw PCM subscriber
       ├-> EncoderNode(raw) -> EncodedPacketHub -> RTSP/录制等 subscriber
       └-> ApmNode -> processedPcmHub
                       -> EncoderNode(APM) -> EncodedPacketHub -> 通话上行 subscriber
```

`AudioFrame` / `EncodedAudioPacket` 在 C 回调边界复制数据并拥有其生命周期；Hub 只分发
`shared_ptr<const ...>`，不做编码、网络 I/O、磁盘 I/O 或阻塞等待。每个 APM / Encoder Node 有自己的
worker 和 5 帧（默认约 50ms）有界输入队列；满时淘汰旧帧，EncoderNode 发现时间戳缺口后重建编码器，
确保不会把缺口两端的 PCM 拼成同一个 20ms Opus 包。

APM 是一条可选支路，而非采集设备的全局状态。因此未来创建/结束通话只会增加/撤销 APM 支路；raw
RTSP 支路持续运行，不会出现“通话一来监控音频断一下”的耦合问题。

#### 2. Hub 发布路径：订阅变更写时复制

初版 `AudioHub::publish()` 为了避免持锁调用外部 callback，每个 10ms PCM 都临时构造
`std::vector<Callback>` 快照。这一语义正确，但只要有订阅者就会在发布热路径分配/释放 callback 数组。

现改为写时复制快照：

```text
subscribe/reset
    -> 持 State::mutex 改订阅表
    -> 重建 immutable CallbackSnapshot

publish（每 10ms）
    -> 持 mutex 复制 shared_ptr<const CallbackSnapshot>
    -> 解锁
    -> 遍历不可变快照调用 callback
```

因此 callback 的 `std::function` 拷贝和 vector 分配只发生在订阅变化时；发布路径保留原有并发语义：
取消订阅和 publish 并发时，已经取到快照的当前一轮 callback 允许执行一次，Node 自己通过运行状态拒绝
停止后的入队。这既不会自锁，也不会让热路径逐帧 malloc。

#### 3. AudioFrame / EncodedAudioPacket payload pool

48kHz 单声道 S16 的 10ms PCM 是 960 bytes。初版每帧存在两层频繁申请：`make_shared<AudioFrame>`
的对象/控制块，以及 `samples.resize()` 的 payload vector；APM 输出和 Opus packet 也有同类行为。

新增：

```text
AudioFramePool
EncodedAudioPacketPool
```

每个 pool 默认预分配 32 个对象和 payload buffer，并实际用于：

```text
raw capture PCM              -> raw AudioFramePool
每个 ApmNode 输出 PCM         -> 该 Node 的 AudioFramePool
每个 EncoderNode 输出编码包    -> 该 Node 的 EncodedAudioPacketPool
```

池满不等待下游归还：立即丢当前实时帧/包，累计计数并在第 1 次及此后每 100 次输出 WARN。这样下游持帧
过久不会反压 ALSA 采集或解码/编码 worker。PCM 丢失会自然形成 EncoderNode 的时间戳缺口，沿用既有的
安全重建策略。

需要明确的边界：pool 已消除 `AudioFrame` / `EncodedAudioPacket` 对象和 payload vector 的高频申请；
由于公开数据流仍使用标准 `shared_ptr`，每次借出时仍有一个标准库 control block。要把它也归零必须把
所有 Hub API 改成自定义 intrusive lease，复杂度和生命周期风险明显增加，当前不做。后续只有在长时间
profiling 证明它是热点时再评估。

新增 `audio_frame_pool_demo`，不依赖声卡，验证 PCM / packet pool 的三条不变式：对象借出时不重复使用、
池满返回空且不阻塞、最后一个 `shared_ptr` 释放后归还并复用同一 payload 地址。

#### 4. C++ 唯一扬声器消费者

新增 `AudioPlaybackPipeline`。它代表 App 内唯一的物理扬声器消费者；一个运行实例只接受一种输入模式，
不能把两条没有统一时钟的 PCM 和压缩包流直接混到同一个设备：

```cpp
bool start(const AudioPlaybackPipelineConfig&);
void stop();
bool push(AudioFramePtr pcm);
bool push(EncodedAudioPacketPtr packet);
```

```text
push(PCM)     -> 固定容量播放队列 -> playback worker -> AudioPlayback -> ALSA
push(packet)  -> 固定容量播放队列 -> playback worker -> AudioDecoder -> AudioPlayback -> ALSA
```

`push()` 只移动 `shared_ptr` 到预分配 ring queue 并唤醒 worker；不会在网络/调用线程执行 Opus 解码或
`snd_pcm_writei()`。播放队列默认最多 100ms，既按总时长限制也按 16 项硬上限限制；满时淘汰最旧项。
它不是 jitter buffer，不处理 RTP 乱序、丢包补偿或混音；这些以后属于 RTSP/WebRTC 通话层。

压缩包路径按 `EncodedAudioPacket::codec` 创建/切换 `AudioDecoder`。decoder 使用 packet 的
`sourceFormat` 解码，随后由 `AudioPlayback` 处理 mono/stereo 设备转换；不能假设 ALSA 实际通道数就是
Opus 源通道数。

未来 AEC 的 reference 应由 playback worker 在“真正写 ALSA 前”将同一份 decoded PCM 投递到通话支路
的 reference queue。此次先不接这一回调，避免在 capture C++ 图尚未接入通话协议前重新产生 manager
之间的反向耦合。

#### 5. ALSA 起播阈值与 XRUN 排查

新 PCM 直放 demo 首测出现连续 XRUN。根因不是 queue 或解码失败：ALSA 默认可能在第一段 10ms PCM 写入
后就开始消耗，软件侧即使已积累 60ms 也尚未写进硬件 buffer；普通 Linux 调度稍晚一个 period 即触发
underrun。

`AudioPlayback` 增加 `requestedStartThresholdFrames`：默认自动使用 `bufferFrames - periodFrames`，并设置
`avail_min = periodFrames`。对 RV1126B 的 48kHz / 10ms period 配置，实际为：

```text
hardware buffer = 3840 frames = 80ms
ALSA start threshold = 3360 frames = 70ms
AudioPlaybackPipeline startup prebuffer = 80ms
```

worker 开始后先快速把预填充写入 ALSA，再由硬件起播；声卡和软件队列都有明确调度余量。首次仍出现一次
XRUN 的原因是 PCM demo 使用连续 `sleep_for(10ms)`，每轮普通线程唤醒误差会累计，3 秒后供给慢于硬件。
demo 已改为 `sleep_until()` 按绝对媒体时间追赶，复测 XRUN 为 0。实际 capture 以 ALSA frame 时钟产出，
不应使用累积 sleep 的方式模拟。

#### 6. 本次 demo 与 RV1126B 验证

新增：

```text
audio_pipeline_demo
    raw PCM -> raw Opus，另挂 APM -> Opus；中途取消 APM 支路

audio_pipeline_stress_demo
    连续创建/销毁 APM -> Opus 支路，同时 raw -> Opus 常驻

audio_frame_pool_demo
    不依赖声卡的 PCM/packet pool 容量与复用验证

audio_playback_pipeline_pcm_demo [seconds]
    440Hz PCM 音调 -> C++ playback pipeline -> ALSA

audio_playback_pipeline_opus_demo [file]
    .mcropus -> C++ playback pipeline -> Opus decoder -> ALSA
```

RV1126B（`root@192.168.1.4`）实测：

```text
audio_frame_pool_demo
    PASS：PCM/packet 均完成满池丢弃与 payload 地址复用验证

audio_pipeline_demo 8
    raw PCM = 100 frame/s，raw Opus = 50 packet/s；4 秒取消 APM 支路后，
    raw Opus 仍从 199 连续增长到 400，APM 停在 174，PASS

audio_pipeline_stress_demo 4
    4 轮 APM -> Opus 创建/销毁全部 PASS；raw Opus 连续增长到 342

audio_playback_pipeline_pcm_demo 3
    accepted=300，dropped=0，playedFrames=144000，playbackFailures=0，XRUN=0

板端新录 pipeline-test.mcropus 后 audio_playback_pipeline_opus_demo
    inputPackets=174，accepted=174，dropped=0，decoded=174，playedFrames=167040，
    decodeFailures=0，PASS
```

`./wsl-build.sh` 与 `./wsl-build-rv1126b.sh` 均通过，`git diff --check` 通过。

#### 7. 后续优化与接入顺序

1. 先以 `AudioPipeline` 接 RTSP 音频的 raw -> Opus subscription；不要让 RTSP 直接操作 ALSA/APM。
2. WebRTC / 对讲接入时，在独立通话支路订阅 APM -> Opus，并把下行 decoded PCM 作为 AEC reference
   投递；jitter buffer、PLC、RTP 时钟都放在协议层。
3. `AudioDecoderOps` 未来增加通用 `reset()`。同一 PCM 格式发生时间戳缺口时，Opus 实现可清空项目自己的
   partial PCM cache 并调用 `OPUS_RESET_STATE`；格式变化仍需 close/init。当前 close/init 逻辑正确且只在
   丢帧时发生，不是性能热点。
4. 在真实 RTSP + 通话并发、长时间运行后用 perf/heap profile 判断是否需要自定义 intrusive lease；在没有
   数据前不把标准 `shared_ptr` 替换成高风险的自定义引用计数。
5. 不重新引入把采集、APM、编码、播放固定串联的 manager；新的业务需求必须通过 `AudioPipeline` 的订阅
   支路或 `AudioPlaybackPipeline` 的唯一扬声器入口表达，避免双架构再次并存。

#### 8. 旧 C manager 清理

在完成 C++ 上/下行管线的板端回归后，检查全仓调用点：`AudioCaptureManager` / `AudioPlaybackManager` 已仅被
自身源码、头文件和两个历史 demo 使用，没有被 IPC App、RTSP server 或其他业务模块依赖。因此删除：

```text
include/core/audio/AudioCaptureManager.h
include/core/audio/AudioPlaybackManager.h
src/core/audio/AudioCaptureManager.c
src/core/audio/AudioPlaybackManager.c
demo/AudioCaptureOpusDemo.c
demo/AudioPlaybackManagerDemo.c
```

同时从 CMake 和 RV1126B 构建脚本移除对应 target。`AudioPipelineDemo` / `AudioPipelineStressDemo` /
`AudioFramePoolDemo` 覆盖新上行图，`AudioPlaybackPipelinePcmDemo` / `AudioPlaybackPipelineOpusDemo` 覆盖新的
唯一扬声器入口；已有 `.opus` packet 文件仍可用两个 Opus playback demo 回归，不为清理旧 manager 新增重复 demo。

## 2026-09-23

### AAC-LC RTSP 音频接入、按客户端启停与接口收口

本次把 IPC 的音频从“录音/编码 demo 闭环”接入正式的多路 RTSP Server。目标不是让每一路视频各自
编码一份音频，而是让整台 IPC 的单路麦克风 PCM 只编码一次，再安全扇出给 `/main`、`/sub` 及其多个
客户端。

#### 1. 最终推流链路

```text
ALSA Capture（实际协商 PCM：当前 RV1126B 为 48kHz / mono / S16_LE）
  -> AudioPipeline raw PCM Hub
  -> 一个 AAC EncoderNode（仅有音频客户端时存在）
  -> RtspAudioPublishSink
  -> Live555RtspServer::pushAacAccessUnit()
  -> /main AudioAccessUnitQueue
  -> /sub  AudioAccessUnitQueue
  -> 各 URL 的 AacAudioSubsession / AudioAccessUnitSource
  -> MPEG4-GENERIC RTP（AAC-hbr）
```

`/main` 与 `/sub` queue 内只各自复制 `EncodedAudioPacketPtr`，共同引用同一份不可变 AAC payload；不复制
压缩字节。进入 live555 `FramedSource` 时才有一次必要的 payload copy 到 live555 的 RTP 发送缓冲。当前
128 kbit/s AAC 的数据量很小，这一次拷贝不是热点。

每个 AAC AU 精确覆盖 1024 samples。`AudioAccessUnitSource` 用 `frameSamples` 推进 RTP 时间线，不能长期
累计整数微秒时长，避免 `1024 / 48000 = 21.333...ms` 的取整漂移。

#### 2. 不再让 App 手填编码器细节

此前 `IpcApp.cpp` 需要手动填写：

```cpp
bitrate = 64000;
frameSamples = 1024;
maxPacketBytes = 2048;
rtspAudio.pcmFormat = audioPipeline.captureFormat();
```

这会让业务层同时知道 AAC 的帧规范、包池大小、实际 PCM 与 SDP 细节，职责不合理。现在新增：

```cpp
enum class AudioBitratePreset { Low, Medium, High };

pipeline.subscribeEncoded(AUDIO_CODEC_AAC, AudioBitratePreset::High, callback);
pipeline.getEncodedStreamInfo(AUDIO_CODEC_AAC, AudioBitratePreset::High, info);
```

Pipeline 内部统一映射策略：

| codec | Low | Medium | High（IPC 默认） |
| --- | ---: | ---: | ---: |
| AAC-LC | 64 kbit/s | 96 kbit/s | 128 kbit/s |
| Opus VOIP | 16 kbit/s | 32 kbit/s | 64 kbit/s |

AAC 内部固定 1024 samples；Opus 内部固定为 20ms。`AudioEncodedStreamInfo` 从已启动 Capture 读取真实协商的
PCM 格式，同时携带 codec、码率、frameSamples。RTSP Server 用这份描述生成 SDP `rtpmap/config` 与 RTP
clock，避免出现“设备实际 44.1kHz，但 SDP 硬编码宣称 48kHz”的格式契约错误。

#### 3. 无客户端时不编码

新增 `RtspAudioPublishSink`，由 `/main`、`/sub` 共同持有。它维护“有音频 PLAY 客户端的 URL 数”，而不是
给每一个 client 创建订阅：

```text
任一 URL audio client：0 -> 1
  -> live555 事件线程只调用 RtspAudioPublishSink::setStreamActive(true)
  -> Sink 的控制 worker 调用 AudioPipeline::subscribeEncoded()
  -> 创建一份 AAC EncoderNode；首包约一个 AAC AU（约 21ms）后可发送

第二个及后续 client / 另一 URL 加入
  -> 只增加引用计数，不重复创建编码器

全部 URL audio client：1 -> 0
  -> 先清理各自的 AudioAccessUnitQueue
  -> Sink worker reset AudioSubscription
  -> AAC EncoderNode 自动停止、析构；ALSA 原始 PCM 采集仍持续运行
```

创建/销毁编码器绝不在 live555 event-loop 执行；事件线程只改轻量状态、唤醒 Sink worker。这样不会因 FDK-AAC
初始化或 `AudioPipeline` 订阅操作阻塞 RTSP 收发。Sink 的编码回调使用独立 `DispatchState`，不捕获 Sink 的
裸 `this`；退订/析构时先关闭 dispatch，再取消 Hub 回调，避免已取得的 Hub callback snapshot 访问悬空对象。

AAC-LC 不需要像视频 IDR 一样为新 client 请求关键帧：AAC AudioSpecificConfig 已通过 SDP 发送，下一包新的
AAC access unit 即可解码。

#### 4. 热路径与可观测性

`Live555RtspServer::pushAacAccessUnit()` 原先每个 AAC 包都创建 `std::vector<PublishStreamPtr>` 快照。48kHz
AAC 每秒约 47 包，虽小但会造成不必要的堆申请。由于 Server 运行期间禁止 `addStream()`，现在在注册 URL 时
预先建立 `m_aacPublishStreams`，发送路径只在 Server 锁保护下遍历稳定表，不再逐 AU 分配 vector。

补齐 APM Node 与 EncoderNode 的 PCM 输入队列溢出统计：队列满时淘汰最旧 10ms PCM 以追实时；首次及每 100
次输出 WARN。EncoderNode 随后会从 PCM timestamp 缺口识别不连续并重建 codec 状态，不能跨时间缺口拼出同一
个 AAC/Opus 包。

板测期间发现日志条件必须排除零值：`droppedCount % 100 == 0` 在零时也成立，会导致没有丢帧却每 10ms 刷
WARN。已修成 `droppedCount != 0 && (...)`，防止日志本身成为实时线程噪声。

#### 5. RV1126B 实测

使用 `root@192.168.1.4`，临时运行新 `ipc_app`，测试结束后恢复 `/oem/usr/bin/multicam_ipc_app` 原自启版本：

```text
空闲启动
  AudioCapture: PCM=48000Hz 1ch S16_LE period=480
  未出现 RtspAudioPublishSink “启动 AAC”日志，证明无人时不编码

ffprobe TCP 拉 /main 音频
  首个 client -> 启动一次 AAC-LC bitrate=128000
  RTP PTS: 0, 1024, 2048, ...
  client 退出 -> 停止一次 AAC 编码

main + sub 重叠拉流
  main 首先启动 AAC 一次
  sub 加入不产生第二次 AAC 启动
  两路退出后只停止一次 AAC 编码
  main 收到 255 包，sub 收到 204 包
```

`cmake --build build/rv1126b-aarch64 --target ipc_app` 与 `./wsl-build.sh` 均通过，`git diff --check` 通过。

#### 6. 后续边界

- 监控 RTSP 目前采用 AAC-LC；Opus 编码支路仍保留给 WebRTC/对讲和相关 demo。
- RTSP AAC queue 默认 8 AU，最坏约 171ms；正常 live555 会即时消费。若后续实测要更低延迟，可评估降到
  4 AU（约 85ms），但不应无界堆积。
- 若 AAC EncoderNode 运行期创建失败，Sink 会保留错误；在全部 client 离开、下一次 0->1 时允许重新尝试，
  不把一次临时失败固化为永久不可恢复状态。

### MPP 实时首包 PTS 修复与 RTSP 客户端 AAC 接收

#### 1. 发现：初始化阶段发送独立 MPP header 是实时 RTP 的错误路径

此前 `MppEncoder::sendFrame()` 第一次送帧前会调用 `rk_mpp_encoder_write_header()`，底层实际执行
`MPP_ENC_GET_HDR_SYNC`。该调用会同步触发 packet callback，并产出一份单独的参数集：H264 为 SPS/PPS，
H265 为 VPS/SPS/PPS。MPP 给这份配置数据的 PTS 固定为 `0`，它不是一张真实输入图像的编码结果。

随后第一张真实帧才携带 V4L2 单调时钟 PTS。在 RTSP 推流中将两者直接相继送入 RTP，会形成：

```text
独立 VPS/SPS/PPS (PTS = 0)
  -> 第一张真实 IDR (PTS = 当前单调时钟，可能已运行数万秒)
```

这会污染视频 RTP 时间线。客户端日志中出现过视频起始时间与音频相差数万秒，根因就是这里，不能通过
播放器侧修复。

为确认 MPP 的实际行为，临时探针分别比较“只送第一张真实帧”和“先 `GET_HDR_SYNC` 再送真实帧”：

```text
H264 只送真实首帧：SPS(7), PPS(8), SEI(6), IDR(5)，PTS=真实帧 PTS
H265 只送真实首帧：VPS(32), SPS(33), PPS(34), SEI(39), IDR(19)，PTS=真实帧 PTS

显式 GET_HDR_SYNC：仅参数集，PTS=0
后续真实 IDR：仅 SEI/IDR（参数集已被前一包提前拿走）
```

因此正式修复（commit `3c4d8f6`）是删除 `MppEncoder` 的 `ensureHeaderWritten()` 与
`headerWritten` 状态；保留 MPP 的 `MPP_ENC_HEADER_MODE_EACH_IDR` 配置，让每个 IDR 自身携带参数集。
`rk_mpp_encoder_write_header()` C 接口仍保留给文件封装或底层诊断，但明确禁止在实时 RTP 首包路径调用。

RV1126B 实测更新后的 `/oem/usr/bin/multicam_ipc_app`：

```text
[RKMPP Encoder] frame=1 ... header=0 intra=1
ffprobe(TCP): video start_time=0.033356, audio start_time=0.000000
```

不再出现 `header=1` 的独立 PTS=0 包，也不再有数万秒的音视频起点错位。

这项修复不等同于解决所有 UDP 首帧问题：静态画面下 H265 IDR 仍可能很大，默认 UDP 客户端接收缓冲不足时
丢失任意 RTP FU 分片，依然会出现 `SPS/PPS does not exist`。TCP 验证正常；提高客户端 UDP 接收缓冲也可
验证该现象。这是首张大 IDR 的传输突发问题，应作为独立事项处理，不能再归因到 header PTS。

#### 2. RTSP 客户端接收 AAC audio track

commit `5e65246` 将拉流端从“只识别一条 H264/H265 视频轨”扩展为可同时接收一条 AAC-LC 音轨：

```text
DESCRIBE SDP
  -> Live555RtspClient 识别 MPEG4-GENERIC audio subsession
  -> SETUP/PLAY 与视频 track 一样建立 RTP 接收链路
  -> AacAudioSink 收到完整 AAC access unit
  -> Stream::onAudioPacket()
  -> 上层 AudioPlaybackPipeline / 录像等自行复制到有界队列
```

`AacAudioSink` 只交付压缩 access unit、SDP 协商出的 PCM 格式、1024 samples 与 presentation timestamp；它不在
live555 事件线程解码或写声卡。`Stream` 的音频回调只借用 packet payload，异步消费者必须用
`EncodedAudioPacketPool` 或自己的有界队列取得所有权，这与视频 NALU 回调的生命周期纪律一致。

新增两个诊断 demo：

- `rtsp_aac_wav_record_demo`：RTSP AAC -> FDK-AAC -> WAV，同时保存 ADTS 码流，隔离 RTP/AAC 与 ALSA 问题；
- `rtsp_audio_playback_demo`：RTSP AAC -> `AudioPlaybackPipeline` -> ALSA。

RK3568（`192.168.1.3`）对 RV1126B（`192.168.1.4`）实测 `rtsp_aac_wav_record_demo` 录制 5 秒：

```text
decodedPackets=234
writtenFrames=239616
poolDropped=0
queueDropped=0
```

`./wsl-build.sh`（全 demo）与 `./wsl-build-ipc.sh`（RV1126B IPC）均通过。

### Live555 IPv4 端口快速重启

live555 默认在 `GenericMediaServer` 建立 RTSP 监听 socket 前关闭 `SO_REUSEADDR`。服务刚停后旧 IPv4 TCP
连接可能仍处于 `TIME_WAIT`，于是 IPv4 `8554` 绑定失败；IPv6 socket 又可以成功，最终进程看似启动但只监听
`:::8554`，IPv4 客户端被拒绝连接。

两个板端静态库以 `ALLOW_RTSP_SERVER_PORT_REUSE=1` 重编译 `GenericMediaServer.o`，保留正常的
`SO_REUSEADDR`。RV1126B 连续 TERM/立即重启后，`netstat -lnt` 已稳定同时显示：

```text
0.0.0.0:8554  LISTEN
:::8554       LISTEN
```

该选项只允许同一服务快速重绑，不允许两个仍存活的 RTSP Server 共用端口。具体构建约束已写入
`third_party/live555/README.md`。

## 2026-09-24：RV1126B H265 大 IDR 的 CBR / VBR / AVBR 对照

### 背景与目标

此前 UDP 拉流排查已确认：静态画面时 H265 的完整 IDR（含 VPS/SPS/PPS）会异常偏大，数百个 RTP FU
分片会在瞬间送入客户端 UDP 接收队列；只要丢掉其中一个分片，整张 IDR 就不可解码。这个问题与 RTP
packetization 无关——RTP 本来就会把大 NALU 正确拆分——重点是减少单张关键帧的突发大小。

本次只比较编码器输出，不让 UDP 丢包干扰结论：RV1126B 上以 TCP 拉取 `/main` 触发编码，并在 MPP packet
callback 中记录**包含参数集的完整 IDR packet** 字节数。测试开关为
`MCR_MPP_ENCODER_DEBUG_IDR=ON`；正常构建默认关闭，统计扫描与日志不会进入实时路径。

### 固定测试条件

| 项目 | 数值 |
|---|---:|
| 板端 | RV1126B（`192.168.1.4`） |
| 视频 | H265 main，1920x1080，NV12，30 fps |
| GOP | 30（约每秒一个 IDR） |
| target bitrate | 5,054,400 bps（`Medium`） |
| `qp_init` | `-1`（MPP 自适应决定初始 QP） |
| 普通帧 QP 范围 | `qp_min=10`，`qp_max=48` |
| I 帧 QP 范围 | `qp_min_i=10`，`qp_max_i=48` |
| I/P QP 差 | `qp_ip=2` |

CBR 保持原量产范围：`bps_min=4,738,500`（15/16 target），`bps_max=5,370,300`（17/16 target）。

VBR 与 AVBR 对照均使用同一宽范围，避免把“范围更大”误当成模式差异：
`bps_min=1,010,880`（20% target），`bps_max=6,065,280`（120% target）。两套 preset 还各自拥有
`bps_target` 比例，当前均为 `1/1`；后续若把 AVBR target 调高，只改 AVBR 分支即可。

### 实测结果（完整 MPP IDR packet）

| 码控模式 | 静态画面稳定 IDR | 持续晃手动态画面稳定 IDR | 结论 |
|---|---:|---:|---|
| CBR | 236–266 KiB | 约 80–105 KiB | 基线；静态关键帧突发偏大。 |
| VBR | 321–331 KiB | 108–137 KiB | 不适合此目标；静态峰值比 CBR 更大。 |
| AVBR | **129–159 KiB**（偶有 174/190 KiB） | **56–75 KiB** | 最能压低单张 IDR 突发，但动态主观画质偏糊，不能直接作为正式默认。 |

AVBR 动态切换进来的第一张 IDR 曾为 273 KiB；这是刚连接/场景状态转换时的过渡值，不与后续稳定段混在一起。
同理，各次编码器刚初始化时的前一两张 IDR 不用于判断稳定策略。

### 当前代码组织与后续调参原则

`mpp_simple.c` 现在保留两套彼此独立的 `RkMppRateControlPreset`：

```text
MCR_MPP_ENCODER_USE_AVBR=ON（默认）  -> AVBR：当前 IPC 默认参数组
MCR_MPP_ENCODER_USE_AVBR=OFF          -> CBR：保留的原量产 bps/QP 基线
```

这样再试 AVBR 的 target、min/max 或 QP 边界时，不会反复手改 CBR 分支，也不会意外改变 CBR 基线。

### 最终默认选择与带宽观察

后续将 AVBR 的 `bps_max` 从 120% target 提高到 156% target（7,884,864 bps），再把
`qp_max` / `qp_max_i` 从实验值 35 恢复为与 CBR 相同的 48。恢复后的实际动态采样中，MPP
报告的平均 QP 为 17--23、实时码率约 1.0--3.6 Mbps，均没有撞到 QP 或 `bps_max` 上限；因此 QP=35
并不是此前主观画质差异的有效限制条件。

更重要的是板端出口流量的实测对照：

| 场景 | CBR | AVBR | 含义 |
|---|---:|---:|---|
| 静止画面 | 约 5 Mbps | 约 1 Mbps | CBR 会持续填满固定预算；AVBR 不会为没有新增画面信息的静态场景浪费网络带宽。 |
| 正常动态 | 约 5 Mbps 固定预算 | 峰值约 3 Mbps | AVBR 仍保留质量预算，但实际复杂度未要求占满 5 Mbps。 |

这不仅减少 IPC 对外带宽，也降低 NVR 多路汇聚时的交换机、网卡和录像写入压力。因此本项目默认切换为
AVBR；CBR 保留为可复现实验的回退分支，而不是删除。

此次拍摄电脑屏幕的纯白背景中，偶见会跳动的细小块状异常。该现象只在平坦高亮、摩尔纹很强的拍屏极端画面中
容易暴露，正常场景中不明显；当前记录为 H265 量化/块划分伪影观察项，不据此继续牺牲 AVBR 的带宽收益。

当前板端暂时运行 AVBR + IDR 调试版以便保留本次实验数据；结束参数实验后必须重新以
`MCR_MPP_ENCODER_DEBUG_IDR=OFF` 构建，再替换量产自启动二进制。

## 2026-09-25：音频运行期可靠性、诊断与断流恢复收尾

### 目标

音频主链已经能采集、AAC/Opus 编解码、RTSP 发布/接收和播放；本次不改变其数据流，专门补齐
“发生异常时能知道、能安全跨过时间缺口、不会把短暂队列波动误判成播放饥饿”的运行期可靠性。

### 1. ALSA 采集端：明确 10ms period 与 80ms hardware buffer

`AudioCaptureConfig` 新增 `requestedBufferFrames`。默认 0 时不再依赖 ALSA 隐式的 buffer 大小，而是在
`period_size_near()` 返回真实 period 后配置 `8 * period`；RV1126B 实测协商结果为：

```text
48000 Hz / mono / S16_LE
period = 480 frames = 10ms
buffer = 3840 frames = 80ms
```

80ms 是 ALSA DMA 环形缓冲提供给普通 Linux 调度的余量，不改变 Pipeline 的 10ms PCM 发布粒度，也不是
网络 jitter buffer。`AudioCapture` 新增累计的 `xrunCount`、`recoveredErrorCount`、`fatalErrorCount`、
`lastError` 和 `running` 状态；`IpcApp` 每 100ms 低频检查，只在计数变化或采集线程异常退出时记录日志。
因此“RTSP 仍活着但采集线程已死”的情况不再静默。

### 2. SIGUSR1 一次性音频快照

为排查曾经出现过的“人声像怪兽、键盘声却正常”问题，新增 `AudioDiagnosticCapture`。向 IPC 发送 `SIGUSR1`
后，主线程启动一次未来 30 秒的捕获：

```text
raw PCM hub  -> raw WAV
AAC hub      -> ADTS .aac + 保留 PTS 的 .mcraudio
```

输出统一位于 `/root/audio-diagnostics/`，含同名 `.txt` 元数据与
`README_AUDIO_DIAGNOSTIC.txt`；触发脚本为 `tools/capture_audio_snapshot.sh`。信号处理函数只置位，订阅、
内存分配和文件写入均在主线程完成。板端快照已验证：约 30 秒、3000 个 10ms PCM block、1406 个 AAC packet、
`timestampGaps=0`。

这份诊断首先用于定位问题处于哪一段：raw WAV 已异常则是 ALSA/硬件采集侧；raw 正常而 AAC 异常则检查编码器；
两者正常而客户端异常则检查 RTSP/RTP、解码或播放侧。

### 3. 热路径收敛

- 删除 APM 每个 10ms PCM block 的 `absolute_sample_sum()` 扫描及累计字段。默认 RTSP 还是 APM bypass，
  这些统计没有运行价值；处理后的 frame/sample 总数仍保留。
- `AudioAccessUnitQueue` 从 `std::vector` 改为 `std::deque`。满队列和出队均 `pop_front()`，不再搬移后续
  `shared_ptr`；队列容量仍是很小的 8 AU，语义不变。
- FDK-AAC Afterburner 实测后固定关闭。RV1126B 独占 64kbps mono AAC 20 秒：开启约 6.3% 单核、关闭约
  4.3% 单核，输出分别 175185 / 175142 bytes，当前没有证明听感收益足以覆盖约 2% 单核的常驻成本。

### 4. PCM 时间戳断裂：轻量 reset，而非 close/init

此前 `EncoderNode` 发现输入 PCM timestamp 跳变后，会完整 `close -> calloc/open/configure -> init` AAC/Opus。
真实跳变通常来自 XRUN 或队列丢帧，正是系统处于瞬时负载压力时；此时额外做销毁/分配/重配置没有必要。

现在 `AudioEncoderOps` 增加 `reset()`：

```text
PCM 格式变化
  -> 完整 close/init（旧 format 的 codec 不能继续使用）

仅 timestamp 断裂、格式未变
  AAC  -> 清应用层未满 1024-sample cache
          + FDK AACENC_CONTROL_STATE(INIT_STATES | RESET_INBUFFER)
          + 空 aacEncEncode() 立即应用内部 history reset
  Opus -> 清未满包 cache + OPUS_RESET_STATE
```

若轻量 reset 意外失败，保留完整重建作为安全降级，不能继续将 PCM 送给状态未知的 codec。

本次顺手修复 FDK-AAC 的边界：首次编码或 history reset 后，FDK 可能已经消费一组 1024 samples 但暂不输出
access unit。旧代码仅在“有输出”时清 `cachedFrames`，导致下一轮 `writableFrames=0` 而空转；现已改为只要
`aacEncEncode()` 成功返回，就释放本应用层的这组 cache。

新增不依赖 ALSA 的 `audio_encoder_reset_demo`，在 RV1126B 实测：

```text
AAC : 断裂前半包 -> reset -> 断裂后完整包，packets=2，firstPts=9000000，PASS
Opus: 断裂前半包 -> reset -> 断裂后完整包，packets=3，firstPts=9000000，PASS
```

首包 PTS 都来自断裂后的 PCM，证明没有把时间缺口两侧数据拼成一包。AAC 在 reset 后允许存在 codec 固有的
priming delay，demo 连续送三组完整 AAC frame 来验证其恢复出包，不把这种正常行为误判为 reset 失败。

### 5. 播放断流后的正确重预缓冲

`AudioPlaybackPipeline::m_playbackStarted` 以前只在 `clearQueueLocked()` 时复位。Gemini 的建议指出播放实际
XRUN 后应重新预缓冲，这个方向正确；但**不能**在 `m_queueCount == 0` 时直接复位：实时 10ms 一包的正常路径
也可能让软件队列瞬间为空，届时会反复等待 80ms，造成周期性延迟和卡顿。

因此底层新增 `audio_playback_write_pcm_ex()` 与 `AudioPlaybackWriteStatus`。只有 ALSA `-EPIPE` 或其它可恢复
write 异常实际发生并被 `prepare/recover` 后，才将 `recoveredFromDiscontinuity` 交给播放管线：

```text
实际 ALSA 断流 -> 底层 recover 成功
                -> PlaybackPipeline 置 m_playbackStarted=false
                -> 后续音频重新积累 startupPrebufferDurationUs（默认 80ms）
                -> 再起播
```

既避免“来一包播一包”后的连锁 XRUN，又不会把正常软件队列波动当成断流。

### 6. 验证与部署状态

- `./wsl-build-ipc.sh` 通过，`git diff --check` 通过；第三方 FDK/WebRTC 头文件仍有既有 unused 参数警告。
- `audio_encoder_reset_demo` 已在 RV1126B (`192.168.1.4`) 运行通过。
- 最新 `ipc_app` 已原子替换到 `/oem/usr/bin/multicam_ipc_app`，本轮测试后按要求没有替用户启动；重启服务或
  执行开机脚本后即使用本版。

### 后续审阅建议

项目当前约 3.3 万行，不应逐行强记。音频审阅优先抓住：`AudioCapture -> raw PCM Hub -> (APM) -> EncoderNode ->
RTSP`，以及 `RTSP -> Decoder -> AudioPlaybackPipeline` 两条主数据流；再阅读三个边界状态：PCM timestamp
断裂、ALSA capture XRUN、ALSA playback XRUN。FDK/Opus/WebRTC 内部实现按输入、输出、线程与失败语义使用即可，
不需要逐行背诵。
