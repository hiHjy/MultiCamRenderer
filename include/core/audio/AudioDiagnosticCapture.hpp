#pragma once

#include "AudioPipeline.hpp"

#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

/*
 * 运行期音频故障取证器。
 *
 * 正常状态不订阅 PCM/AAC、不占用编码器、不写文件。调用 startOneShot() 后才临时订阅：
 *
 *   raw PCM Hub -> 30 秒 WAV
 *   AAC EncoderNode -> 30 秒 .mcraudio（带 PTS）+ .aac（ADTS，便于 ffplay）
 *
 * 所有文件写入都在本对象的 monitor 线程完成；AudioHub 回调只把数据复制到本次短暂的
 * 内存缓冲，不能在 AudioCapture 或 Encoder worker 线程中写盘。
 */
struct AudioDiagnosticCaptureConfig {
    uint32_t durationSeconds = 30;
    /* 故障后通常需要重启才能恢复声音，故默认放 root 持久目录，不能放 /tmp。 */
    std::string outputDirectory = "/root/audio-diagnostics";
    AudioCodec encodedCodec = AUDIO_CODEC_AAC;
    AudioBitratePreset bitratePreset = AudioBitratePreset::Medium;
};

class AudioDiagnosticCapture {
public:
    explicit AudioDiagnosticCapture(const AudioDiagnosticCaptureConfig& config = {});
    ~AudioDiagnosticCapture();

    AudioDiagnosticCapture(const AudioDiagnosticCapture&) = delete;
    AudioDiagnosticCapture& operator=(const AudioDiagnosticCapture&) = delete;

    /*
     * 动态开始一次取证。采满 durationSeconds 后自动退订、落盘并结束。
     * 正在进行一次取证时返回 false，避免两个编码订阅和两组文件相互混杂。
     */
    bool startOneShot(AudioPipeline& pipeline);
    void cancel();
    bool isCapturing() const;
    std::string lastError() const;

private:
    struct Session;

    static void recordPcm(const std::weak_ptr<Session>& session, AudioFramePtr frame);
    static void recordEncoded(const std::weak_ptr<Session>& session, EncodedAudioPacketPtr packet);
    void monitorMain(std::shared_ptr<Session> session);
    void finishSession(const std::shared_ptr<Session>& session, bool writeFiles);
    void setErrorLocked(const std::string& message);

    AudioDiagnosticCaptureConfig m_config;
    mutable std::mutex m_mutex;
    std::condition_variable m_monitorCv;
    AudioSubscription m_pcmSubscription;
    AudioSubscription m_encodedSubscription;
    std::thread m_monitorThread;
    bool m_capturing = false;
    bool m_cancelRequested = false;
    std::string m_lastError;
};
