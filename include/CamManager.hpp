#pragma once

#include "FrameHub.hpp"
#include "V4L2CameraSource.hpp"
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <atomic>
#include <condition_variable>

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
    // startCamera()/stopCamera() 不直接在调用线程里碰 V4L2 状态机，
    // 而是投递内部命令，由 poll 线程统一执行 STREAMON/STREAMOFF，
    // 避免和 DQBUF/QBUF 在不同线程交叉。
    // 当前不要在旧 lease 完全释放前复用同一个 cameraId。
    // 后续应改成内部生成 cameraId，或给 cameraId 增加 generation 校验。
    bool addCamera(const CameraConfig& config);
    bool delCamera(int cameraId);
    bool addFrameSink(int cameraId, std::shared_ptr<Sink> sink);
    bool addSinkForHub(int cameraId, std::shared_ptr<Sink> sink);
    // 返回 true 表示控制命令已投递成功，不表示硬件已经完成 STREAMON/STREAMOFF。
    bool startCamera(int cameraId);
    bool stopCamera(int cameraId);
    bool startAllCameras();
    void stopAllCameras();

    // pollOnce 会阻塞等待摄像头 fd 就绪，并在内部 dequeue/requeue 帧。
    // 当前版本只适合单线程或外部保证 cameraMap 不会被并发修改的场景。
    bool pollOnce(int timeoutMs = 2000);
    void run(int timeoutMs = 2000);
    void requestStop();
	void startPolling();
	void shutdownPolling();
    std::string lastError() const;

private:
    // 内部命令队列只承载会改变 V4L2 fd 状态机的操作。
    // addCamera/delCamera 仍保持当前 shared_ptr 管理引用语义。
    enum class CommandType {
        StartCamera,
        StopCamera,
    };

    struct Command {
        CommandType type;
        int cameraId = -1;
    };

    void setError(const std::string& message);
    void clearError();
    bool postCommand(Command command);
    bool drainCommands();
    bool executeCommand(const Command& command);
    void postReturnedFrame(int cameraId, int bufferIndex);
    void notifyReturnEvent();
    bool drainReturnEvent();
    bool drainReturnedFrames();

private:
    std::mutex m_camChangeMutex;
	std::condition_variable m_camCv;
    std::unordered_map<int, CameraSlot> m_cameraMap {};
    mutable std::mutex m_errorMutex;
    std::string m_lastError;
    std::unordered_map<int, std::shared_ptr<FrameHub>> m_frameHubMap;
	std::atomic_bool m_stopRequested {false};
	std::atomic_bool m_running {false};
	std::atomic_bool m_hasPendingCommand {false};
    std::mutex m_returnMutex;
    std::queue<std::pair<int, int>> m_returnQueue;
    std::mutex m_commandMutex;
    std::queue<Command> m_commandQueue;
    int m_returnEventFd = -1;
    std::mutex m_threadMutex;
	std::thread m_pollThread {};
};
