#pragma once

#include "FrameHub.hpp"
#include "V4L2CameraSource.hpp"

#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>

class CamManager {
public:
    CamManager();
    ~CamManager();

    enum class CameraState {
        Created,
        Ready,
        Streaming,
        Stopped,
        Error
    };

    struct CameraConfig {
        int cameraId = -1;
        std::string devicePath;
        int width = 0;
        int height = 0;
        int fps = 0;
        PixelFormat format = PixelFormat::Auto;
        int bufferCount = 4;
        std::string dmaHeapPath = "/dev/dma_heap/system";
    };

	
    struct CameraSlot {
        CameraConfig config;
        std::shared_ptr<V4L2CameraSource> source;
        CameraState state = CameraState::Created;
        std::string lastError;
    };

    // 当前内部用 shared_ptr 持有摄像头，pollOnce() 会复制 shared_ptr 快照，
    // delCamera() 只从 map 中摘除管理引用，不主动 stop 快照中的 V4L2 fd。
    // 已经 DQBUF/publish 出去的帧由 FrameLease 继续保护 source 生命周期；
    // 最后一个引用释放后，V4L2CameraSource 析构时统一清理 fd/DMA buffer。
    // 这支持基础运行中删除；当前不要在旧 lease 完全释放前复用同一个 cameraId。
    // 后续应改成内部生成 cameraId，或给 cameraId 增加 generation 校验。
    bool addCamera(const CameraConfig& config);
    bool delCamera(int cameraId);
    bool addFrameSink(int cameraId, std::shared_ptr<Sink> sink);
    bool addSinkForHub(int cameraId, std::shared_ptr<Sink> sink);
    bool startAll();
    void stopAll();

    // pollOnce 会阻塞等待摄像头 fd 就绪，并在内部 dequeue/requeue 帧。
    // 当前版本只适合单线程或外部保证 cameraMap 不会被并发修改的场景。
    bool pollOnce(int timeoutMs = 2000);
    void run(int timeoutMs = 2000);
    void requestStop();

    const std::string& lastError() const;

private:
    void setError(const std::string& message);
    void postReturnedFrame(int cameraId, int bufferIndex);
    void notifyReturnEvent();
    bool drainReturnEvent();
    bool drainReturnedFrames();

private:
    std::mutex m_camChangeMutex;
    std::unordered_map<int, CameraSlot> m_cameraMap {};
    std::string m_lastError;
    std::unordered_map<int, std::shared_ptr<FrameHub>> m_frameHubMap;
    bool m_stopRequested = false;
    std::mutex m_returnMutex;
    std::queue<std::pair<int, int>> m_returnQueue;
    int m_returnEventFd = -1;
};
