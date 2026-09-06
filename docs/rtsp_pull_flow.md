# RTSP 拉流全链路

本文对应当前 `RtspStream` 实现，目标不是解释 RTSP 协议的所有细节，而是把本项目中一条 H.264/H.265 压缩 NALU 如何到达上层稳定 NV12 DMA-BUF 讲清楚。

## 0. 先看完整数据流和线程归属

```text
调用线程 / StreamManager
  RtspStream::start()
    ├─ 创建 DecodeWorker 线程
    └─ 创建 live555 事件线程

live555 事件线程
  RTSP DESCRIBE → SETUP → PLAY
  RTP 包 → live555 重组完整 NALU → AnnexBSink 补 Annex-B 起始码
  → Stream::onPacket() → 复制到压缩队列

DecodeWorker 线程
  压缩队列 → MPP 解码临时 NV12 → RGA copy 到 Stream 的 DMA pool
  → readyQueue<FramePacket>

调用线程 / StreamManager
  tryGetFrame() → FrameHub → Sink
```

三个线程的边界必须记住：

- **live555 事件线程**只处理 RTSP/RTP 和压缩 NALU 投递，不能做耗时解码、RGA、磁盘 I/O。
- **DecodeWorker**是本路唯一访问 MPP 和 RGA 的线程。
- **Manager/Sink 线程**只消费已经稳定的 `FramePacket`，不能接触 live555 或 MPP 的临时内存。

## 1. 程序入口

示例入口是 `drm/RtspStreamDemo.cpp`：

```cpp
RtspStream stream("rtsp://192.168.1.5:8554/live", 2);
stream.start();

FramePacket packet;
while (stream.tryGetFrame(packet)) {
    // 正式工程中交给 StreamManager / FrameHub / Sink
}
```

第二个构造参数是 `readyQueueCapacity`，当前默认也是 `2`。它只表示“已经解码、等待上层取走的裸帧”容量，不是 RTP 缓冲，也不是压缩码流缓存。

`FramePacket` 定义在 `include/VideoFrame.hpp`：

```cpp
struct FramePacket {
    VideoFrame frame;
    std::shared_ptr<FrameLease> lease;
};
```

`frame` 描述 dma-fd 和图像 layout；`lease` 保证最后一个使用者释放后才归还该 DMA buffer。

## 2. RtspStream：启动接收和解码两条线程

入口是 `src/RtspStream.cpp` 的 `RtspStream::start()`：

```cpp
startDecodeWorker();
m_client->start(m_url,
    [this](VideoCodec codec, uint8_t* data, size_t size, uint64_t timestampUs) {
        onPacket(toMppCodec(codec), data, size, timestampUs);
    });
```

这里先启动 DecodeWorker，再启动 live555。这样服务端在 `PLAY` 后立即发送 RTP 时，压缩 NALU 已有消费者。

`VideoCodec` 是传输层通用编码类型，`toMppCodec()` 位于 `include/hw/MppTypes.hpp`，负责统一转换成硬件 MPP 的 `MppCodec`。业务代码不能各自再写一份 H264/H265 的转换 switch。

## 3. live555 的异步 RTSP 状态机

`Live555RtspClient::start()` 会新建事件线程。线程入口为 `drm/live555/Live555RtspClient.cpp` 的 `eventThreadMain()`：

```cpp
setupLive555();
env->taskScheduler().doEventLoop(&eventLoopWatchVariable);
```

`doEventLoop()` 等待网络 socket、定时器和 `EventTrigger`，事件到了才调用回调；它不是同步阻塞地依次执行 DESCRIBE、SETUP、PLAY。

状态机是：

```text
sendDescribeCommand()
  → continueAfterDESCRIBE()
  → MediaSession::createNew() 解析 SDP
  → setupNextSubsession()
  → subsession.initiate()
  → sendSetupCommand()
  → continueAfterSETUP()
  → sink->startPlaying()
  → sendPlayCommand()
  → continueAfterPLAY()
```

### DESCRIBE

DESCRIBE 响应主体是 SDP。live555 将它解析为：

```text
MediaSession
  └─ MediaSubsession（一个 track）
```

`setupNextSubsession()` 只选择 `medium=video` 且 `codec=H264/H265` 的第一条视频 track；音频和其他编码跳过。一条 `RtspStream` 当前只对应一路视频。

### SETUP

`subsession.initiate()` 创建本地 RTP/RTCP 接收链路和 UDP socket；`sendSetupCommand()` 将客户端接收参数告诉服务端。

SETUP 成功后，必须先：

```cpp
subsession.sink->startPlaying(*subsession.readSource(), ...);
```

让 Sink 登记 `getNextFrame()`，再发送 PLAY。这样首个 RTP 包到来时已经有接收者。

### PLAY

PLAY 成功后服务端正式发送 RTP。此后 live555 事件线程不断从 socket 收包并驱动 `readSource()` 到 Sink。

## 4. RTP 到完整 Annex-B NALU

一个大 NALU 可能在网络层被拆成 FU-A/FU RTP 包：

```text
一个 H264 NALU
  → 多个 RTP 包
  → 网络
  → live555 重组
  → AnnexBSink 收到完整 NALU
```

项目不自己解析 FU-A。`subsession.readSource()` 已经过 live555 的 RTP 解包器处理，`AnnexBSink` 收到的是完整 NALU。

`AnnexBSink::continuePlaying()` 调用：

```cpp
fSource->getNextFrame(receiveBuffer_.data() + 4, ...);
```

buffer 前四字节预先写入 `00 00 00 01`。因此 `afterGettingFrame()` 调给上层的是：

```text
00 00 00 01 + 完整 NALU payload
```

这就是可直接送 MPP 的 Annex-B NALU，不是 RTP 包，也不需要上层再拼 FU-A。

`afterGettingFrame()` 返回前会再次调用 `continuePlaying()`。live555 是拉取式：Sink 每处理完一条 NALU，就登记请求下一条。

## 5. 为什么压缩 NALU 必须复制

`AnnexBSink::receiveBuffer_` 会在下一次 `getNextFrame()` 时复用。因此 live555 回调中的 `data` 只在当前回调期间有效。

`Stream::onPacket()` 最终进入 `DecodeWorker::enqueue()`：

```cpp
packet.annexB.assign(data, data + size);
```

此复制不可省。复制完成后 live555 可立即继续接收下一条 NALU，MPP 解码不会阻塞网络事件线程。

压缩队列最大为 512 条。压缩 NALU 队列满时不能像裸帧队列一样随意丢弃，因为一条 NALU 可能只是一个多-slice 图像的一部分，或者会破坏后续参考链；当前会记录错误并等待后续恢复策略。

## 6. DecodeWorker 与 MPP

`DecodeWorker::workLoop()` 从压缩队列取出 NALU；首次收到 H264/H265 时调用：

```cpp
decoder.init(packet.codec);
```

MPP decoder 对 H264/H265 开启 `split_parse`，可顺序接收带 Annex-B 起始码的完整 NALU：

```cpp
input.va = packet.annexB.data();
input.bytesUsed = packet.annexB.size();
input.timestampUs = packet.timestampUs;
decoder.sendPacket(input);
```

MPP 解析码流后通过 frame callback 输出 NV12。一个压缩 NALU 不保证恰好对应一个回调裸帧；例如参数集不输出图像，B 帧重排序也可能影响输出时机。以 MPP callback 为准。

MPP callback 给出的 `VideoFrame` 是 **MPP 临时帧视图**，仅在 callback 返回前有效，不能直接入队或交给外部。

## 7. RGA 稳定化 copy 和 DMA pool

`Stream::makeStableFramePacket()` 是临时 MPP 帧变稳定帧的边界：

```text
MPP 临时 NV12 dma-buf
  → RGA copy
  → Stream 自己的稳定 NV12 dma-buf
```

步骤：

1. `ensureOutputPool()` 检查 MPP 输出的 `width/height/stride/heightStride/format`。
2. 第一次输出或 layout 改变时，新建 4 块 buffer 的 `DmaBufferPool`。
3. `pool->acquireFrame()` 取得一块空闲输出 DMA buffer。
4. 填写该 buffer 的真实 layout，调用 `rga.copy(decodedFrame, *outputFrame)`。
5. 用 `FrameLease` 包装 `pool->releaseFrame(outputFrame)`，形成稳定 `FramePacket`。

分辨率改变时会切换到新 pool。旧 FramePacket 的 lease 捕获旧 pool，因此已交给 Sink 的旧 dma-buf 不会提前释放。

## 8. readyQueue、低延迟与丢帧统计

稳定 `FramePacket` 由 `enqueueDecodedFrame()` 放入 `readyQueue`：

```cpp
while (m_readyQueue.size() >= m_readyQueueCapacity) {
    m_readyQueue.pop_front();
}
m_readyQueue.push_back(std::move(packet));
```

满时淘汰最旧裸帧，目标是低延迟显示。它发生在解码之后，不会损坏 H264/H265 参考链。

目前有两类 `LOG_WARN` 统计：

- `readyQueue淘汰`：新裸帧到来时，旧裸帧尚未被上游取走。
- `pool被下游lease占满`：4 块输出 DMA buffer 都被 Hub/Sink 持有，当前新裸帧无法落池。

pool 不够时，先释放一条最旧的 ready 帧并重试 acquire；不会清空整个队列。日志最多每秒输出一次，stop 时强制输出最终累计值。

监控预览允许 readyQueue 丢旧裸帧；录像不能走 readyQueue，应从压缩码流复制后的独立 recordQueue 写 Annex-B/封装文件。

## 9. 停止顺序

`RtspStream::stop()` 的顺序：

```text
Live555RtspClient::stop()
  → EventTrigger 唤醒 live555 doEventLoop
  → 关闭 Sink，发送 TEARDOWN，停止 NALU 生产

Stream::stopDecodeWorker()
  → 停止接受压缩包，清空未解码压缩队列
  → join DecodeWorker
  → MPP deinit
```

必须先停 live555 生产者，再停 DecodeWorker 消费者，避免 worker 已析构后仍有 NALU 回调。

## 10. 项目规则：禁止 C++ 异常

本项目的实时音视频、硬件和网络链路 **禁止使用 C++ exception**：

```text
禁止：throw、try、catch
```

原因：

- ARM64 实时链路不以异常作为错误控制流，避免不可预期的栈展开、代码体积和性能成本。
- live555/MPP/RGA 的异步回调边界不适合让异常跨越；异常跨 C/C++ 回调边界还可能导致未定义行为或直接终止。
- 错误必须由所属模块就地处理：返回 `bool` / 空指针，写入 `lastError()`，并按需要使用项目 `LOG_WARN` 或 `LOG_ERROR`。

回调签名应优先表达可预期错误。例如需要向上游报告失败时，应让回调返回 `bool` 或通过明确的 ErrorCallback/reportError 接口上报，不能依赖抛异常。
