#pragma once

#include "V4L2CameraSource.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

class CamManager {
public:
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
        V4L2CameraSource::CamConfig videoConfig;
        int bufferCount = 4;
        std::string dmaHeapPath = "/dev/dma_heap/system";
    };

    struct CameraSlot {
        CameraConfig config;
        std::unique_ptr<V4L2CameraSource> source;
        CameraState state = CameraState::Created;
        std::string lastError;
    };

    // 第一版并发约束：
    // addCamera/delCamera 只能在 run()/pollOnce() 没有并发执行时调用。
    // 当前内部用 unique_ptr 持有摄像头，pollOnce() 会临时复制裸指针快照；
    // 如果运行中删除摄像头，快照里的裸指针可能悬空。
    // 后续若要支持热插拔，应改成事件队列或 shared_ptr 快照。
    bool addCamera(const CameraConfig& config);
    bool delCamera(int cameraId);

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

private:
    std::mutex m_camChangeMutex;
    std::unordered_map<int, CameraSlot> m_cameraMap;
    std::string m_lastError;
    bool m_stopRequested = false;
};
