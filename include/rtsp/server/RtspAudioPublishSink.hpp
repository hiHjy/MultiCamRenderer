#pragma once

#include "AudioPipeline.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class Live555RtspServer;

/*
 * RTSP 的公共音频编码入口。
 *
 * main/sub 的 audio track 共同持有一个 RtspAudioPublishSink，因此也共同持有一份
 * AudioPipeline AAC/Opus 编码支路：任一路第一个 audio client PLAY 时才订阅编码，
 * 全部 URL 的最后一个 audio client 离开时自动退订。原始 PCM 采集不受影响。
 *
 * setStreamActive() 由 live555 事件线程调用，但它只修改轻量状态并唤醒 worker；真正的
 * subscribeEncoded()/退订及编码器创建销毁都在 m_workerThread，不能阻塞 RTSP event loop。
 */
class RtspAudioPublishSink final {
public:
    struct Config {
        AudioCodec codec = AUDIO_CODEC_AAC;
        /* High 是业务默认：AAC-LC=128k，Opus=64k；具体映射在 AudioPipeline 内部。 */
        AudioBitratePreset bitratePreset = AudioBitratePreset::High;
    };

    RtspAudioPublishSink(AudioPipeline& pipeline, Live555RtspServer& server, Config config);
    ~RtspAudioPublishSink();

    RtspAudioPublishSink(const RtspAudioPublishSink&) = delete;
    RtspAudioPublishSink& operator=(const RtspAudioPublishSink&) = delete;

    /*
     * 一个 URL 的音频客户端数发生 0->1 或 1->0 时调用。
     * 共享该 Sink 的 main/sub 会在内部汇总为全局活跃 URL 数，只有全局边界才启停编码订阅。
     */
    void setStreamActive(bool active);

    /* 构造时由已启动的 AudioPipeline 解析，供 RTSP Server 建 SDP；不需要 App 手填 PCM 格式。 */
    AudioEncodedStreamInfo streamInfo() const;
    bool isReady() const;
    bool isEncoding() const;
    std::string lastError() const;

private:
    struct DispatchState {
        explicit DispatchState(Live555RtspServer& streamServer)
            : server(streamServer)
        {
        }

        Live555RtspServer& server;
        std::atomic_bool acceptingPackets {false};
    };

    void workerMain();
    void setErrorLocked(const std::string& message);

private:
    AudioPipeline& m_pipeline;
    Live555RtspServer& m_server;
    const Config m_config;
    const std::shared_ptr<DispatchState> m_dispatchState;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_workerThread;
    AudioEncodedStreamInfo m_streamInfo {};
    unsigned m_activeStreamCount = 0;
    bool m_encodingRequested = false;
    bool m_activationAttempted = false;
    bool m_encoding = false;
    bool m_stopping = false;
    std::string m_lastError;
};
