#include "CamManager.hpp"
#include "Log.hpp"
#include "Sink.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_stopRequested {false};

extern "C" void onStopSignal(int)
{
    g_stopRequested.store(true);
}

class StressSink final : public Sink {
public:
    void onFrame(FramePacket packet) override
    {
        ++m_frameCount;

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_holdNext && !m_heldPacket.has_value()) {
            m_holdNext = false;
            m_heldPacket = std::move(packet);
            m_holdCv.notify_all();
        }
    }

    uint64_t frameCount() const
    {
        return m_frameCount.load();
    }

    void holdNextFrame()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_holdNext = true;
    }

    bool waitUntilHolding(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_holdCv.wait_for(lock, timeout, [this] {
            return m_heldPacket.has_value();
        });
    }

    void releaseHeldFrame()
    {
        std::optional<FramePacket> packet;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            packet.swap(m_heldPacket);
        }
        // 在锁外析构 FrameLease；其回调会投递 return event 给 CamManager。
        packet.reset();
    }

private:
    std::atomic<uint64_t> m_frameCount {0};
    std::mutex m_mutex;
    std::condition_variable m_holdCv;
    bool m_holdNext = false;
    std::optional<FramePacket> m_heldPacket;
};

bool waitForFrames(const StressSink& sink,
                   uint64_t baseline,
                   uint64_t requiredFrames,
                   std::chrono::seconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (sink.frameCount() >= baseline + requiredFrames) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

int parsePositive(const char* text, int fallback)
{
    if (text == nullptr) {
        return fallback;
    }
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    return end != text && value > 0 ? static_cast<int>(value) : fallback;
}

PixelFormat parseFormat(const std::string& text)
{
    if (text == "mjpeg" || text == "MJPEG") {
        return PixelFormat::MJPEG;
    }
    return PixelFormat::YUYV;
}

bool runRecoveryCase(CamManager& manager,
                     int cameraId,
                     StressSink& sink,
                     CamManager::RecoveryMode mode,
                     const std::string& label)
{
    const uint64_t before = sink.frameCount();
    LOG_INFO("CamRecoveryStress", "开始 " << label << "，beforeFrames=" << before);
    if (!manager.requestCameraRecovery(cameraId, mode, "压力测试: " + label)) {
        LOG_ERROR("CamRecoveryStress", label << " 请求失败: " << manager.lastError());
        return false;
    }

    if (!waitForFrames(sink, before, 15, std::chrono::seconds(15))) {
        LOG_ERROR("CamRecoveryStress", label << " 超时，恢复后没有持续收到帧: "
                                                << manager.lastError());
        return false;
    }

    LOG_INFO("CamRecoveryStress", label << " 通过，afterFrames=" << sink.frameCount());
    return true;
}

bool runHeldLeaseRecoveryCase(CamManager& manager, int cameraId, StressSink& sink)
{
    LOG_INFO("CamRecoveryStress", "开始持有旧 FrameLease 后的自动恢复测试");
    sink.holdNextFrame();
    if (!sink.waitUntilHolding(std::chrono::seconds(3))) {
        LOG_ERROR("CamRecoveryStress", "没有拿到可持有的 FrameLease");
        return false;
    }

    const uint64_t before = sink.frameCount();
    if (!manager.requestCameraRecovery(cameraId,
                                       CamManager::RecoveryMode::RestartStream,
                                       "压力测试: 持有 lease 时轻恢复")) {
        LOG_ERROR("CamRecoveryStress", "持有 lease 的恢复请求失败: " << manager.lastError());
        sink.releaseHeldFrame();
        return false;
    }

    // 等超过 CamManager 的 2 秒告警门限，确认恢复路径不会不安全地重用旧 DMA buffer。
    std::this_thread::sleep_for(std::chrono::seconds(3));
    sink.releaseHeldFrame();

    if (!waitForFrames(sink, before, 15, std::chrono::seconds(15))) {
        LOG_ERROR("CamRecoveryStress", "旧 lease 归还后没有恢复出帧: " << manager.lastError());
        return false;
    }

    LOG_INFO("CamRecoveryStress", "持有旧 FrameLease 后的自动恢复测试通过");
    return true;
}

bool runStopStartLeaseCase(CamManager& manager, int cameraId, StressSink& sink)
{
    LOG_INFO("CamRecoveryStress", "开始 stop/start 跨代 lease 防护测试");
    sink.holdNextFrame();
    if (!sink.waitUntilHolding(std::chrono::seconds(3))) {
        LOG_ERROR("CamRecoveryStress", "没有拿到 stop/start 测试所需的 FrameLease");
        return false;
    }

    const uint64_t before = sink.frameCount();
    if (!manager.stopCamera(cameraId) || !manager.startCamera(cameraId)) {
        LOG_ERROR("CamRecoveryStress", "stop/start 命令投递失败: " << manager.lastError());
        sink.releaseHeldFrame();
        return false;
    }

    // StartCamera 必须等待此 lease 归还，不能先 STREAMON 再让旧 index 回到新队列。
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    sink.releaseHeldFrame();

    if (!waitForFrames(sink, before, 15, std::chrono::seconds(15))) {
        LOG_ERROR("CamRecoveryStress", "stop/start 后没有恢复出帧: " << manager.lastError());
        return false;
    }

    LOG_INFO("CamRecoveryStress", "stop/start 跨代 lease 防护测试通过");
    return true;
}

// 真实 USB 拔插验证不主动注入故障，也不把“暂时没有帧”判定为失败。
// CamManager 自己会在 poll 错误后按退避策略重建设备；此处只持续报告现状，
// 方便人工拔出、重新插入并观察恢复后的帧率。
void runPhysicalDisconnectTest(const StressSink& sink, const CamManager& manager)
{
    LOG_INFO("CamRecoveryStress", "进入 physical 模式：现在可物理拔插摄像头；"
                                   "恢复期间会持续重试，按 Ctrl+C 退出");

    uint64_t previousFrames = sink.frameCount();
    while (!g_stopRequested.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));

        const uint64_t currentFrames = sink.frameCount();
        const uint64_t delta = currentFrames - previousFrames;
        if (delta == 0) {
            LOG_WARN("CamRecoveryStress", "过去 2 秒未收到帧；CamManager 正在等待或恢复，error="
                                                << manager.lastError());
        } else {
            LOG_INFO("CamRecoveryStress", "physical capture fps=" << delta / 2.0
                                                   << " totalFrames=" << currentFrames);
        }
        previousFrames = currentFrames;
    }
}

} // namespace

int main(int argc, char** argv)
{
    const bool physicalMode = argc > 1 && std::string(argv[1]) == "physical";
    const int argumentOffset = physicalMode ? 2 : 1;
    const std::string device = argc > argumentOffset ? argv[argumentOffset] : "/dev/video0";
    const std::string formatText = argc > argumentOffset + 1 ? argv[argumentOffset + 1] : "yuyv";
    const int width = argc > argumentOffset + 2 ? parsePositive(argv[argumentOffset + 2], 640) : 640;
    const int height = argc > argumentOffset + 3 ? parsePositive(argv[argumentOffset + 3], 480) : 480;
    const int fps = argc > argumentOffset + 4 ? parsePositive(argv[argumentOffset + 4], 30) : 30;
    const int cycles = argc > argumentOffset + 5 ? parsePositive(argv[argumentOffset + 5], 2) : 2;

    std::signal(SIGINT, onStopSignal);
    std::signal(SIGTERM, onStopSignal);

    LOG_INFO("CamRecoveryStress", "开始" << (physicalMode ? "物理拔插测试" : "恢复压力测试")
                                            << " device=" << device
                                            << " format=" << formatText
                                            << " size=" << width << 'x' << height
                                            << " fps=" << fps
                                            << (physicalMode ? "" : " cycles=" + std::to_string(cycles)));

    CamManager manager;
    CamManager::CameraConfig config {};
    config.devicePath = device;
    config.width = width;
    config.height = height;
    config.fps = fps;
    config.format = parseFormat(formatText);
    config.bufferCount = 4;

    const int cameraId = manager.addCamera(config);
    if (cameraId < 0) {
        LOG_ERROR("CamRecoveryStress", "addCamera 失败: " << manager.lastError());
        return 1;
    }

    auto sink = std::make_shared<StressSink>();
    if (!manager.addFrameSink(cameraId, sink)) {
        LOG_ERROR("CamRecoveryStress", "addFrameSink 失败: " << manager.lastError());
        return 1;
    }

    manager.startPolling();
    if (!manager.startCamera(cameraId) || !waitForFrames(*sink, 0, 20, std::chrono::seconds(10))) {
        LOG_ERROR("CamRecoveryStress", "初始起流失败: " << manager.lastError());
        manager.shutdownPolling();
        return 1;
    }

    bool passed = true;
    if (physicalMode) {
        runPhysicalDisconnectTest(*sink, manager);
    } else {
        for (int cycle = 1; cycle <= cycles && passed; ++cycle) {
            LOG_INFO("CamRecoveryStress", "========== cycle " << cycle << '/' << cycles << " ==========");
            passed = runRecoveryCase(manager,
                                     cameraId,
                                     *sink,
                                     CamManager::RecoveryMode::RestartStream,
                                     "模拟 POLLERR/DQBUF 轻恢复");
            if (passed) {
                passed = runRecoveryCase(manager,
                                         cameraId,
                                         *sink,
                                         CamManager::RecoveryMode::ReopenDevice,
                                         "模拟 POLLHUP/POLLNVAL 完整重建");
            }
            if (passed) {
                passed = runHeldLeaseRecoveryCase(manager, cameraId, *sink);
            }
            if (passed) {
                passed = runStopStartLeaseCase(manager, cameraId, *sink);
            }
        }
    }

    sink->releaseHeldFrame();
    (void)manager.delCamera(cameraId);
    manager.shutdownPolling();

    if (physicalMode) {
        LOG_INFO("CamRecoveryStress", "物理拔插测试已结束 totalFrames=" << sink->frameCount());
    } else {
        LOG_INFO("CamRecoveryStress", "恢复压力测试" << (passed ? "全部通过" : "失败")
                                                << " totalFrames=" << sink->frameCount());
    }
    return passed ? 0 : 2;
}
