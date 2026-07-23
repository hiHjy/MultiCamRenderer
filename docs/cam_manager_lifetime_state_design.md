# CamManager 生命周期与状态设计笔记

这份笔记记录 `CamManager` 当前设计里的一个关键问题：摄像头对象的生命周期、`poll` 阻塞、动态增删摄像头之间的关系。

当前第一版能跑通两路摄像头，但还不是完整的运行时管理器。明天建议先把这个问题想清楚并改一版，再继续往 RGA / StreamHub 推。

## 1. 当前结构

现在 `CamManager` 内部大致是：

```cpp
std::unordered_map<int, std::unique_ptr<V4L2CameraSource>> m_cameraMap;
```

`pollOnce()` 里为了避免拿锁阻塞在 `poll()`，会先复制一份快照：

```cpp
std::vector<pollfd> fds;
std::vector<V4L2CameraSource*> cameras;

{
    std::lock_guard<std::mutex> lock(m_camChangeMutex);
    for (auto& item : m_cameraMap) {
        V4L2CameraSource* camera = item.second.get();
        fds.push_back(...);
        cameras.push_back(camera);
    }
}

poll(fds.data(), fds.size(), timeoutMs);

cameras[i]->dequeueFrame(frame);
```

这样做的好处是：`poll()` 阻塞时不持有 `m_camChangeMutex`，外部不会因为 `poll()` 卡住而无法调用管理接口。

但它有一个风险。

## 2. 风险点：裸指针快照不拥有对象

`cameras` 里保存的是裸指针：

```cpp
V4L2CameraSource*
```

裸指针只是地址，不延长对象生命周期。

如果另一个线程在 `poll()` 阻塞期间调用：

```cpp
delCamera(cameraId);
```

而 `delCamera()` 做了：

```cpp
m_cameraMap.erase(it);
```

因为 map 里存的是 `unique_ptr`，erase 会立刻析构对应的 `V4L2CameraSource`。

这时 `pollOnce()` 之前保存的裸指针就变成悬空指针：

```cpp
cameras[i]->dequeueFrame(frame); // use-after-free 风险
```

所以当前第一版必须有一个约束：

```text
run()/pollOnce() 执行期间，不允许并发 addCamera()/delCamera()。
```

这个约束现在可以接受，因为项目当前目标是跑通主链路，不是热插拔。

## 3. 为什么不一直拿锁 poll

一种直觉做法是：

```cpp
lock();
poll(...);
dequeueFrame(...);
unlock();
```

这样不会有裸指针悬空问题，因为 map 在整个过程中都被锁住。

但这会带来另一个问题：`poll()` 可能阻塞很久，例如 2000ms。期间所有管理操作都会被卡住：

```text
addCamera
delCamera
stopAll
requestStop
```

后面如果 `poll()` 后还接 RGA copy / StreamHub publish，锁持有时间会更长。

所以长期来看，不应该靠“大锁包住所有逻辑”解决问题。

## 4. CameraSlot 的意义

当前 map 只保存摄像头对象：

```cpp
cameraId -> V4L2CameraSource
```

这更像一个容器，不像完整管理器。

更好的结构是：

```cpp
enum class CameraState {
    Created,
    Ready,
    Streaming,
    Stopped,
    Error
};

struct CameraSlot {
    CameraConfig config;
    std::unique_ptr<V4L2CameraSource> source;
    CameraState state = CameraState::Created;
    std::string lastError;
};

std::unordered_map<int, CameraSlot> m_cameraMap;
```

这表示 map 里保存的不只是对象，而是一整路摄像头的运行记录：

```text
cameraId -> 配置 + 对象 + 状态 + 错误信息
```

这样 `CamManager` 才真正知道每路摄像头现在是什么状态、是否能 start、是否出错、能否重启。

## 5. 推荐的状态含义

第一版状态不需要太复杂：

```cpp
enum class CameraState {
    Created,
    Ready,
    Streaming,
    Stopped,
    Error
};
```

含义：

```text
Created:
  slot 已创建，但初始化流程尚未完成。

Ready:
  openDevice / configure / setupDmaImportBuffers 都成功，等待 start。

Streaming:
  已经 STREAMON，可以进入 poll。

Stopped:
  曾经初始化成功，但当前已经 stop。

Error:
  这一路摄像头发生错误，lastError 保存原因。
```

对应操作关系：

```text
addCamera 成功:
  Created -> Ready

startAll / startCamera 成功:
  Ready/Stopped -> Streaming

stopAll / stopCamera:
  Streaming -> Stopped

任意关键错误:
  -> Error
```

## 6. CameraSlot 能解决什么

`CameraSlot` 本身还不能完全解决并发删除导致的裸指针悬空问题。

但它解决了另一个更基础的问题：状态不再散落在对象和外部逻辑里。

有了 `CameraSlot`，后续可以更自然地做：

```cpp
bool startCamera(int cameraId);
bool stopCamera(int cameraId);
bool restartCamera(int cameraId);
CameraState cameraState(int cameraId) const;
std::string cameraError(int cameraId) const;
```

它也为后面的事件队列打基础。

## 7. 最终方向：事件队列

如果后面要支持运行中动态增删摄像头，比较稳的方式是事件队列。

核心原则：

```text
外部线程不直接修改 m_cameraMap。
外部线程只投递命令。
CamManager 的 run 线程自己处理命令、修改 map、poll fd、处理帧。
```

例如：

```cpp
enum class CommandType {
    AddCamera,
    RemoveCamera,
    StartCamera,
    StopCamera,
    StopAll,
    Shutdown
};

struct Command {
    CommandType type;
    int cameraId = -1;
    CameraConfig config;
};
```

外部调用：

```cpp
postAddCamera(config);
postRemoveCamera(cameraId);
postShutdown();
```

内部 run 线程：

```cpp
while (!quit) {
    drainCommands();
    rebuildPollFds();
    poll(...);
    handleReadyFrames();
}
```

这样 `m_cameraMap` 只被 manager 线程操作，就不会出现另一个线程在 `poll()` 时把对象删掉的问题。

## 8. eventfd / pipe 唤醒

事件队列还有一个细节：如果 manager 正在 `poll()`，外部投递了命令，它怎么立刻醒来？

专业做法是增加一个唤醒 fd：

```text
eventfd 或 pipe
```

把这个 fd 也加入 `poll`：

```text
camera fd 0
camera fd 1
camera fd 2
wakeup fd
```

外部线程投递命令后，写一下 wakeup fd，`poll()` 立即返回，manager 就能处理命令。

这个模型就是常见的 reactor / event-loop 思路。

## 9. 明天建议先做什么

不要一口气上完整事件队列。明天建议先做中间层：

1. 引入 `CameraState`。
2. 引入 `CameraSlot`。
3. 把 map 改成：

```cpp
std::unordered_map<int, CameraSlot> m_cameraMap;
```

4. `addCamera()` 内部创建 slot：

```text
Created -> Ready
```

5. `startAll()` 只启动 `Ready/Stopped` 的 camera：

```text
Ready/Stopped -> Streaming
```

6. `stopAll()`：

```text
Streaming -> Stopped
```

7. `pollOnce()` 只 poll `Streaming` 的 camera。

8. 保留当前并发约束：

```text
暂不支持 run()/pollOnce() 期间并发 add/del。
```

这一步做完以后，`CamManager` 会从一个简单容器升级成真正的状态管理器。

## 10. 长期演进顺序

推荐演进路线：

```text
阶段 1:
  unique_ptr + 简单 mutex + 明确不支持并发 add/del

阶段 2:
  CameraSlot + CameraState

阶段 3:
  pollOnce 中接 RGA copy / StreamHub publish

阶段 4:
  Command queue

阶段 5:
  eventfd/pipe 唤醒 poll，支持运行中 add/del/start/stop
```

当前最重要的是先把主链路跑稳：

```text
多路 V4L2 -> CamManager poll -> RGA copy -> StreamHub ring buffer
```

热插拔和完整事件队列可以后面再处理。

