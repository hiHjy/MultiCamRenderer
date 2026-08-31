#pragma once

#include "DmaBufferPool.hpp"
#include "FrameHub.hpp"
#include "MppDecoder.hpp"
#include "V4L2CameraSource.hpp"
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include "VideoFrame.hpp"
// 1. CamManager 对外只发布裸帧
// 2. MJPEG 是 camera 内部输入格式，不暴露给 DisplaySink
// 3. poll 线程不做 MPP/RGA 重活，只投递 FramePacket
// 4. decode worker 持有 input lease，解码 + RGA copy 完再释放
// 5. MPP 原始输出不 publish
// 6. RGA copy 后的稳定 output pool 才 publish
// 7. output lease 负责归还 decode worker 的稳定 pool



class CamManager {
public:
    // 每路 MJPEG camera 独占一个 DecodeWorker。
    // 输入侧只保留最新一帧，避免解码排队造成延迟堆积；
    // 输出侧使用 DmaBufferPool，publish 后由 output lease 归还池子。
    class DecodeWorker {
    public:
        DecodeWorker(int cameraId,
                     int outputBufferCount,
                     size_t outputBufferSize,
                     const std::string& dmaHeapPath,
                     std::weak_ptr<FrameHub> hub);
        ~DecodeWorker();

        DecodeWorker(const DecodeWorker&) = delete;
        DecodeWorker& operator=(const DecodeWorker&) = delete;

        DecodeWorker(DecodeWorker&&) = delete;
        DecodeWorker& operator=(DecodeWorker&&) = delete;

        bool initialized() const;
        bool postFrame(FramePacket packet);
        void shutdown();
        const std::string& lastError() const;

    private:
        void workerLoop();
        void processFrame(FramePacket packet);
        void setError(const std::string& message);

    private:
        int m_cameraId = -1;
        std::weak_ptr<FrameHub> m_hub;
        std::shared_ptr<DmaBufferPool> m_outputPool;
        MppDecoder m_decoder {};
        bool m_initialized = false;
        bool m_stopping = false;
        uint64_t m_droppedFrames = 0;
        std::mutex m_mutex;
        std::condition_variable m_cv;
        std::optional<FramePacket> m_pendingPacket;
        std::thread m_workerThread;
        std::string m_lastError;
    };

    CamManager();
    ~CamManager();

    enum class CameraState {
        Created,
        Ready,
        Streaming,
        Stopped,
        Deleting,
        Error
    };

    struct CameraConfig {
        std::string devicePath;
        int width = 0;
        int height = 0;
        int fps = 0;
        PixelFormat format = PixelFormat::Auto;
        int bufferCount = 4;
        std::string dmaHeapPath = "/dev/dma_heap/system";
        // addCamera() 成功后回填为 V4L2 实际单个输入 buffer 容量。
        // 外部传入配置时不用填写。
        size_t bufferCapacity = 0;
    };

	
    struct CameraSlot {
        CameraConfig config;
        std::shared_ptr<V4L2CameraSource> source;
        CameraState state = CameraState::Created;
        std::string lastError;
		std::unique_ptr<DecodeWorker> decodeWorker = nullptr;
    };

    // 当前内部用 shared_ptr 持有摄像头，pollOnce() 会复制 shared_ptr 快照，
    // delCamera() 会先关闭 FrameHub，再由 poll 线程摘除 map 管理引用。
    // 这样 delCamera() 返回后，不再向对应 sink 发布新帧。
    // 它不直接 stop V4L2 fd；如果有快照/FrameLease 仍持有 source shared_ptr，
    // V4L2CameraSource 会等最后一个引用释放后再析构清理。
    // 已经 DQBUF/publish 出去的帧由 FrameLease 继续保护 source 生命周期；
    // 最后一个引用释放后，V4L2CameraSource 析构时统一清理 fd/DMA buffer。
    // startCamera()/stopCamera() 不直接在调用线程里碰 V4L2 状态机，
    // 而是投递内部命令，由 poll 线程统一执行 STREAMON/STREAMOFF，
    // 避免和 DQBUF/QBUF 在不同线程交叉。
    // addCamera() 内部单调生成 cameraId 并返回，避免外部复用 id 时
    // 旧 FrameLease 的归还事件误命中新摄像头。
    // 返回 >= 0 表示成功生成的 cameraId，返回 -1 表示失败，错误信息见 lastError()。
    int addCamera(const CameraConfig& config);
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
 
    void requestStop();
	void startPolling();
	void shutdownPolling();
    std::string lastError() const;

private:


    // 内部命令队列承载需要和 DQBUF/QBUF 串行的操作。
    enum class CommandType {
        StartCamera,
        StopCamera,
        DeleteCamera,
    };

    struct Command {
        CommandType type;
        int cameraId = -1;
    };

	bool pollOnce(int timeoutMs = 2000);
    void run(int timeoutMs = 2000);
    void setError(const std::string& message);
    void clearError();
    int allocateCameraIdLocked();
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
    int m_nextCameraId = 0;
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
