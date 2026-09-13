#include "Log.hpp"
#include "Stream.hpp"
#include "StreamManager.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

class RecoveryProbeStream final : public Stream {
public:
    RecoveryProbeStream()
        : Stream(1)
    {
    }

    bool start() override
    {
        return startDecodeWorker();
    }

    bool stop() override
    {
        stopDecodeWorker();
        return true;
    }

    void inject(VideoCodec codec, const std::vector<uint8_t>& annexB, uint64_t timestampUs)
    {
        onPacket(codec, annexB.data(), annexB.size(), timestampUs);
    }
};

bool waitForState(StreamManager& manager,
                  int streamId,
                  StreamManager::StreamState expected,
                  std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        StreamManager::StreamState state {};
        if (manager.getStreamState(streamId, state) && state == expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

} // namespace

int main()
{
    // 这个 demo 不验证压缩数据是否能实际解码；它验证恢复门控本身：
    // 1. 无参数集/IDR 时五秒后要变成 Error；
    // 2. 一个合并 Annex-B access unit [SPS][PPS][IDR] 要能重新打开恢复边界；
    // 3. 随后 StreamManager 必须从 Error 回到 Streaming。
    StreamManager manager;
    auto stream = std::make_shared<RecoveryProbeStream>();
    const int streamId = manager.addStream(stream);
    if (streamId < 0 || !manager.startStream(streamId)) {
        LOG_ERROR("StreamRecoveryDemo", "启动 probe stream 失败: " << manager.lastError());
        return 1;
    }

    LOG_INFO("StreamRecoveryDemo", "等待恢复门控超时（约 5 秒）");
    if (!waitForState(manager, streamId, StreamManager::StreamState::Error, std::chrono::seconds(7))) {
        LOG_ERROR("StreamRecoveryDemo", "未收到 recovery timeout Error: " << manager.lastError());
        (void)manager.stopStream(streamId);
        return 2;
    }

    // 这里故意将三个 H264 NALU 合在一次 onPacket()，验证恢复等待期会拆完整 Annex-B
    // access unit，而不是只检查第一个 SPS 后永久等待 PPS/IDR。
    const std::vector<uint8_t> combinedRecoveryAccessUnit {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1E, 0xF4, 0x05, 0x01, 0xEC,
        0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x06, 0xE2,
        0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00,
    };
    LOG_INFO("StreamRecoveryDemo", "注入合并 [SPS][PPS][IDR] access unit，验证恢复状态回转");
    stream->inject(VideoCodec::H264, combinedRecoveryAccessUnit, 123456789ULL);

    if (!waitForState(manager, streamId, StreamManager::StreamState::Streaming, std::chrono::seconds(2))) {
        LOG_ERROR("StreamRecoveryDemo", "恢复边界到达后未回到 Streaming: " << manager.lastError());
        (void)manager.stopStream(streamId);
        return 3;
    }

    LOG_INFO("StreamRecoveryDemo", "通过：timeout Error 与合并 Annex-B 恢复回 Streaming 均符合预期");
    (void)manager.stopStream(streamId);
    return 0;
}
