#pragma once

#include "AudioApm.h"
#include "AudioCapture.h"
#include "AudioCodec.h"
#include "AudioFrame.hpp"
#include "AudioHub.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/* AudioPipeline 的采集与 Node 队列公共配置。 */
struct AudioPipelineConfig {
    AudioCaptureConfig capture {};
    size_t defaultNodeQueueCapacity = 5; // 5 x 10ms，优先追实时，不积压旧语音。
    /* 每个 raw/APM PCM 支路独立持有的预分配帧数。池满时丢当前帧而不阻塞采集线程。 */
    size_t pcmFramePoolCapacity = 32;
    /* 每个 Encoder Node 独立持有的预分配压缩包数，单包容量取 encoderConfig.maxPacketBytes。 */
    size_t encodedPacketPoolCapacity = 32;

    AudioPipelineConfig();
};

/* 请求原始 PCM 或某个 APM 配置处理后的 PCM。 */
struct AudioPcmRequest {
    bool useApm = false;
    AudioApmConfig apmConfig {};
    uint32_t apmWarmupDiscardDurationMs = 500;

    AudioPcmRequest();
};

/* 请求某一路 PCM 经指定编码器后的压缩包。 */
struct AudioEncodedRequest {
    bool useApm = false;
    AudioApmConfig apmConfig {};
    uint32_t apmWarmupDiscardDurationMs = 500;
    AudioEncoderConfig encoderConfig {};
    size_t inputQueueCapacity = 0; // 0 表示使用 AudioPipelineConfig 的默认值。

    AudioEncodedRequest();
};

/*
 * 外部订阅的 RAII 句柄。析构或 reset() 后取消回调；句柄同时保持所需 Node 存活。
 * 必须保存返回值，不能 subscribe 后立即丢弃。
 */
class AudioSubscription {
public:
    AudioSubscription() = default;
    AudioSubscription(const AudioSubscription&) = delete;
    AudioSubscription& operator=(const AudioSubscription&) = delete;
    AudioSubscription(AudioSubscription&&) noexcept = default;
    AudioSubscription& operator=(AudioSubscription&&) noexcept = default;
    ~AudioSubscription();

    void reset();
    bool valid() const;

private:
    friend class AudioPipeline;

    AudioSubscription(std::function<void()> unsubscribe, std::shared_ptr<void> keepAlive);

    std::function<void()> m_unsubscribe;
    std::shared_ptr<void> m_keepAlive;
};

/*
 * App 级音频图的上行入口。
 *
 * 它只拥有一个 C AudioCapture，并把采到的干净 PCM 复制为 AudioFrame 后发布。外部通过
 * subscribePcm()/subscribeEncoded() 声明需要的结果，内部按需创建并复用 APM/Encoder Node。
 * 这不是全局单例：App 显式拥有唯一实例，并把 shared_ptr 或引用传给需要订阅的模块。
 */
class AudioPipeline {
public:
    using PcmCallback = std::function<void(AudioFramePtr)>;
    using EncodedPacketCallback = std::function<void(EncodedAudioPacketPtr)>;

    explicit AudioPipeline(const AudioPipelineConfig& config = AudioPipelineConfig());
    ~AudioPipeline();

    AudioPipeline(const AudioPipeline&) = delete;
    AudioPipeline& operator=(const AudioPipeline&) = delete;

    /* App 启动时显式调用；成功后底层 AudioCapture 的实际格式可由 captureFormat() 查询。 */
    bool startCapture();
    void stopCapture();
    bool isCaptureRunning() const;
    AudioPcmFormat captureFormat() const;

    /* 订阅 raw PCM，或订阅某个 APM 配置产生的 PCM。回调运行在 Hub 发布线程，必须快速返回。 */
    AudioSubscription subscribePcm(const AudioPcmRequest& request, PcmCallback callback);

    /* 订阅 raw/APM PCM 经编码后的压缩包。编码和回调都不运行在 AudioCapture 线程。 */
    AudioSubscription subscribeEncoded(const AudioEncodedRequest& request,
                                       EncodedPacketCallback callback);

    std::string lastError() const;

private:
    class ApmNode;
    class EncoderNode;
    struct KeepAlive;

    static void onCapturedPcm(const AudioPcmFrame* frame, void* userData);
    void publishCapturedPcm(const AudioPcmFrame& frame);

    std::shared_ptr<ApmNode> findOrCreateApmNodeLocked(const AudioPcmRequest& request);
    std::shared_ptr<EncoderNode> findOrCreateEncoderNodeLocked(
        const std::shared_ptr<AudioPcmHub>& inputHub,
        const AudioEncodedRequest& request);
    std::vector<std::shared_ptr<ApmNode>> collectApmNodesLocked();
    std::vector<std::shared_ptr<EncoderNode>> collectEncoderNodesLocked();
    bool startNodeLocked(const std::shared_ptr<ApmNode>& node);
    bool startNodeLocked(const std::shared_ptr<EncoderNode>& node);
    void setErrorLocked(const std::string& message);

    mutable std::mutex m_mutex;
    AudioPipelineConfig m_config;
    AudioCapture* m_capture = nullptr;
    AudioPcmFormat m_captureFormat {};
    snd_pcm_uframes_t m_capturePeriodFrames = 0;
    std::shared_ptr<AudioPcmHub> m_rawPcmHub;
    std::shared_ptr<AudioFramePool> m_rawFramePool;
    uint64_t m_droppedRawFramesByPool = 0;
    std::vector<std::weak_ptr<ApmNode>> m_apmNodes;
    std::vector<std::weak_ptr<EncoderNode>> m_encoderNodes;
    bool m_captureRunning = false;
    std::string m_lastError;
};
