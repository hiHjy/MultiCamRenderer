# live555 RTSP 客户端运行逻辑

本文只讲 `Live555RtspClient` 的协议和数据接收部分；MPP、RGA、DMA pool 等后续解码链路请看 `docs/rtsp_pull_flow.md`。

对应实现：

```text
include/rtsp/client/Live555RtspClient.hh
src/rtsp/client/Live555RtspClient.cpp
include/rtsp/client/AnnexBSink.hh
src/rtsp/client/AnnexBSink.cpp
```

## 1. 这个封装负责什么

`Live555RtspClient` 只负责 RTSP 控制协议、RTP 接收与 H.264/H.265 NALU 重组：

```text
RTSP DESCRIBE / SETUP / PLAY
  → RTP 包
  → live555 H264/H265 RTP 解包和 FU 分片重组
  → 完整 NALU
  → AnnexBSink 补 00 00 00 01
  → AnnexBNaluCallback(codec, data, size, timestampUs)
```

它不做 access unit 组帧，不做 MPP 解码，也不做录像。

回调里的 `data` 是带 4 字节 Annex-B 起始码的完整 NALU；它只在回调期间有效。上层若交给另一个线程，必须复制压缩数据。

## 2. 对象关系

```text
Live555RtspClient
  └─ Impl
       ├─ eventThread                 live555 专属事件线程
       ├─ BasicTaskScheduler          socket/定时器/事件触发器调度器
       ├─ BasicUsageEnvironment       live555 运行环境和错误信息入口
       └─ Client : RTSPClient
            ├─ PullClientState
            │    ├─ MediaSession*
            │    ├─ MediaSubsessionIterator*
            │    └─ 当前 MediaSubsession*
            └─ owner: Impl&           静态回调回到封装对象的桥梁
```

live555 的 RTSP 回调是 C 风格的静态函数，只会传入 `RTSPClient*`。因此项目定义了 `Client : RTSPClient`，并保存 `Impl& owner`：

```text
RTSPClient* 回调参数
  → static_cast<Client*>()
  → client->owner
  → Impl::continueAfterDESCRIBE/SETUP/PLAY()
```

这个模式来自 live555 官方 `testRTSPClient.cpp`：官方的 `ourRTSPClient + StreamClientState` 对应项目的 `Client + PullClientState`。

## 3. start()：只保证事件线程已启动

调用：

```cpp
client.start("rtsp://192.168.1.5:8554/live", callback);
```

`start()` 保存 URL 和回调，创建 `eventThread` 后立刻返回。它返回 `true` 不表示网络握手已经成功；DESCRIBE、SETUP、PLAY 是后续事件循环中的异步步骤，失败信息通过 `lastError()` 获取。

事件线程入口为：

```cpp
setupLive555();
env->taskScheduler().doEventLoop(&eventLoopWatchVariable);
cleanupLive555();
```

`doEventLoop()` 持续处理 RTSP TCP 响应、RTP/RTCP socket、定时任务和用户事件；`eventLoopWatchVariable` 变成非零时退出。

## 4. 创建 Client 的三个参数

```cpp
client = Client::createNew(*env, url_.c_str(), *this);
```

```text
*env          UsageEnvironment&。live555 的事件、日志、错误环境，不复制。
url_.c_str()  const char* RTSP URL，告诉 RTSPClient 要连接哪个地址。
*this         Impl&。Client 保存它，供静态异步回调回到本封装。
```

`Client` 构造时再调用 live555 的 `RTSPClient` 基类构造函数：

```cpp
RTSPClient(env, url, 1, "Live555RtspClient", 0, -1)
```

其中 `1` 是日志详细度，`0` 表示不使用 HTTP tunnel，`-1` 表示由 live555 自己创建 RTSP TCP socket。

## 5. DESCRIBE：SDP 变成 MediaSession

创建 client 后立即异步发送：

```cpp
client->sendDescribeCommand(continueAfterDESCRIBE);
```

服务端回复的 body 是 SDP。例如：

```text
m=video 0 RTP/AVP 96
a=rtpmap:96 H264/90000
a=control:track1
```

`continueAfterDESCRIBE()` 中：

```cpp
session = MediaSession::createNew(client.envir(), resultString);
iterator = new MediaSubsessionIterator(*session);
setupNextSubsession(client);
```

`MediaSession::createNew()` 是在**客户端本地解析 SDP**，生成每条媒体轨的描述对象；它不会创建服务端的媒体轨。

关系如下：

```text
MediaSession
  ├─ MediaSubsession：video / H264 / track1
  ├─ MediaSubsession：audio / AAC  / track2
  └─ ...
```

`MediaSession` 不是 `std::vector`，但内部拥有多个 subsession。`MediaSubsessionIterator::next()` 用于逐条取出。

下面两句等价于“取到当前 track 指针，再起一个方便使用的引用”：

```cpp
client.state.subsession = client.state.iterator->next();
MediaSubsession& subsession = *client.state.subsession;
```

它不是把 `session` 当成 `subsession`。

## 6. SETUP：本地准备接收端，再与服务端协商传输

当前 `setupNextSubsession()` 会遍历 SDP 中的 track，但只选择第一路 H264/H265 video：

```text
取下一条 MediaSubsession
  → 是不是 video + H264/H265？不是则跳过
  → 是否已经选择过视频？是则跳过
  → initiate()
  → sendSetupCommand()
  → return，等待异步 SETUP 响应
```

### 6.1 `subsession.initiate()`

```cpp
subsession.initiate();
```

这一步在客户端本地按 SDP 创建 RTP/RTCP 接收链路。UDP 模式下，会准备本地 RTP/RTCP 接收端口和对应的 RTP source。

它还没有通知服务端。

### 6.2 `sendSetupCommand()`

```cpp
client.sendSetupCommand(subsession, continueAfterSETUP,
                        False, requestRtpOverTcp ? True : False);
```

这才是发给服务端的 RTSP SETUP，含义是：

```text
我要拉这个 track。
UDP：请把 RTP/RTCP 发到我刚准备的 client port。
TCP：请把 RTP/RTCP 交织在当前 RTSP TCP 连接中。
```

服务端回复 SETUP 成功，代表它接受了该 track 的传输协商；随后进入 `continueAfterSETUP()`。

所有需要的 track 都 SETUP 完后，才发送：

```cpp
client.sendPlayCommand(*session, continueAfterPLAY);
```

PLAY 表示通知服务端开始正式发 RTP。

## 7. SETUP 成功后创建 Sink

`continueAfterSETUP()` 中的：

```cpp
MediaSubsession& subsession = *client.state.subsession;
subsession.sink = AnnexBSink::createNew(...);
subsession.sink->startPlaying(*subsession.readSource(), ...);
```

这里的 `subsession` 正是刚刚完成 SETUP 的 track。

`readSource()` 输出的是 live555 已完成 RTP 解包、H264/H265 FU 分片重组后的数据。`AnnexBSink` 再补 Annex-B 起始码并调用项目回调。

必须在 PLAY 前调用 `startPlaying()`：这样 Sink 已预先登记 `getNextFrame()`，服务端刚开始发 RTP 时已有接收者。

`AnnexBSink::continuePlaying()` 处理完当前 NALU 后必须再次调用 `getNextFrame()`。这是 live555 的拉取式数据模型：Sink 明确表示“我处理完了，请给我下一帧”。

### 7.1 AnnexBSink 的内部结构

`AnnexBSink` 继承 live555 的 `MediaSink`，是客户端侧的媒体数据消费者：

```text
subsession.readSource()（live555 的 RTP 解包结果）
  → AnnexBSink : MediaSink
  → Live555RtspClient::AnnexBNaluCallback
```

头文件中最重要的成员是：

```cpp
std::vector<uint8_t> receiveBuffer_;
VideoCodec codec_;
NaluCallback naluCallback_;
ErrorCallback errorCallback_;
```

`receiveBuffer_` 是反复复用的单 NALU 接收 buffer。构造时分配 `maxNaluBytes + 4` 字节，并预先在前四字节写入：

```text
00 00 00 01
```

每次请求下一帧时，live555 从第 5 字节开始写 NALU payload：

```cpp
fSource->getNextFrame(receiveBuffer_.data() + 4, ...);
```

因此上层回调得到的内存布局始终是：

```text
00 00 00 01 | NALU payload
^ data       ^ data + 4
```

当前 `Live555RtspClient` 传入的最大值是 2 MiB。若单个 NALU 超过 buffer，live555 会通过 `numTruncatedBytes` 报告截断；该 NALU 不会交给上层，错误会写入 `lastError()`，随后仍继续请求下一条。

### 7.2 为什么有静态和成员两层 afterGettingFrame()

live555 的 `getNextFrame()` 只接受 C 风格函数指针，因此需要静态回调：

```cpp
static void afterGettingFrame(void* clientData, ...);
```

调用 `getNextFrame()` 时把 `this` 作为 `clientData` 传入。静态函数只做转发：

```cpp
static_cast<AnnexBSink*>(clientData)->afterGettingFrame(...);
```

成员函数才执行实际处理：

```text
检查是否截断
→ 将 timeval 转为 timestampUs
→ naluCallback_(codec, data, size, timestampUs)
→ continuePlaying()
```

`naluCallback_` 是 `Live555RtspClient::start()` 的调用者传入的函数。它运行在 live555 事件线程；不能在其中阻塞等待、同步写文件或长时间解码。实时链路应快速复制压缩数据后投递给后续 worker。

## 8. 未来支持音频时怎么改

SDP 解析阶段本来就会生成 audio `MediaSubsession`；不需要在 DESCRIBE 回调里手工创建新的 audio subsession。

要做的是在 `setupNextSubsession()` 中把音频也选中，按同样的时序：

```text
video subsession → initiate → SETUP → VideoSink
audio subsession → initiate → SETUP → AudioSink
所有目标 track 均 SETUP 完成 → PLAY
```

当前单个 `state.subsession` 在串行 SETUP 模式下足够：它总是指向“正在等待 SETUP 响应的 track”。因此仅为了在 `continueAfterSETUP()` 创建 AudioSink，不必额外保存 `audioSubsession`。

但工程化支持音视频后，应将当前的 `selectedCodec`、`hasSelectedVideo` 扩展为每轨状态，例如：

```cpp
struct TrackState {
    MediaSubsession* subsession = nullptr;
    MediaSink* sink = nullptr;
    TrackType type; // Video / Audio
    // video codec 或 audio codec 的描述
};
```

音频不能直接复用 `AnnexBSink`：H264/H265 是 NALU，AAC、G711 等音频 RTP payload 有自己的格式、配置和解码路径。

## 9. UDP、TCP 与多客户端共享 source

客户端的 `requestRtpOverTcp` 只是在 SETUP 时提出传输请求：

```text
UDP：低延迟，少量丢包等待后续帧/IDR 恢复，适合局域网实时预览。
TCP：适合 UDP 被防火墙/NAT 拦截的网络；丢包重传会带来队头阻塞和延迟累积。
```

服务端是否接受由 RTSP SETUP 协商决定；标准 live555 server 的 `OnDemandServerMediaSubsession` 支持 RTP-over-TCP，不需要为 TCP 单独再写一套 source。

这和服务端是否共享 source 是独立问题。配套服务端的 `VideoSubsession` 构造为：

```cpp
OnDemandServerMediaSubsession(env, true);
```

其中 `true` 是 `reuseFirstSource`：

```text
第一个客户端 PLAY
  → 创建 AnnexBSource
  → AnnexBSource 从共享 AnnexBFrameQueue pop NALU

后续客户端 PLAY
  → 复用首个 stream source / stream state
  → live555 向新增客户端发送同一条实时流
```

这对于实时编码器是正确的。若每客户端都创建一个 `AnnexBSource` 并从同一队列 `popNalu()`，多个客户端会互相抢 NALU，导致彼此缺帧。

共享的是码流读取与 RTP 流状态；不共享的是网络发送。服务端仍需分别把数据发送给每个客户端，因此总出网带宽会随客户端数量增长。慢 TCP 客户端也更容易积累延迟。

## 10. stop()：跨线程退出事件循环

外部线程调用 `stop()` 时：

```cpp
eventLoopWatchVariable = 1;
scheduler->triggerEvent(stopTrigger, this);
```

watch variable 是退出条件；`triggerEvent()` 的作用是唤醒可能阻塞在 `select()/poll()` 的 `doEventLoop()`。`stopEventCallback()` 本身不做业务逻辑。

事件循环退出后依次：关闭各 Sink、发送 TEARDOWN、关闭 `RTSPClient`、回收 `UsageEnvironment`、删除 `TaskScheduler`。顺序必须保持 client 在 env/scheduler 之前释放。

## 11. 读代码的推荐顺序

```text
Live555RtspClient::start()
→ eventThreadMain()
→ setupLive555()
→ continueAfterDESCRIBE()
→ setupNextSubsession()
→ continueAfterSETUP()
→ AnnexBSink::continuePlaying()
→ AnnexBSink::afterGettingFrame()
→ Live555RtspClient::stop()
→ cleanupLive555()
```
