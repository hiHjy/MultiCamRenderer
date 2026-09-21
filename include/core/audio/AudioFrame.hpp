#pragma once

#include "AudioTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

/*
 * C++ 音频图中流动的、拥有 PCM 数据的一帧。
 *
 * AudioCapture / AudioDecoder 的 C 回调只在回调期间借出 data 指针；AudioFrame 在边界
 * 复制那份数据，之后可通过 shared_ptr 安全分发给多个异步 Node。10ms 的 48kHz 单声道
 * S16 PCM 只有 960 bytes，因此这里一次复制换取明确生命周期是划算的。
 */
struct AudioFrame {
    std::vector<uint8_t> samples;
    AudioPcmFormat format {};
    size_t frames = 0;
    uint64_t timestampUs = 0;

    AudioPcmFrame pcmView() const;
    static std::shared_ptr<const AudioFrame> copyFrom(const AudioPcmFrame& frame);
};

using AudioFramePtr = std::shared_ptr<const AudioFrame>;

/*
 * 固定大小 PCM 帧池。
 *
 * AudioFrame 的 samples buffer 在池创建时一次性申请；运行期 acquire/copy 只复用 buffer。
 * 返回的 shared_ptr 最后一个所有者释放时，custom deleter 会把对象还回池。shared_ptr 自身
 * 的控制块仍由标准库管理，但最频繁的 PCM object 与 payload 分配已经被消除。
 *
 * 池满时 copyFrom() 返回 nullptr，调用者必须丢弃当前实时帧，绝不能等待下游归还。
 */
class AudioFramePool {
public:
    AudioFramePool(size_t capacity, size_t maximumSampleBytes);

    AudioFramePtr copyFrom(const AudioPcmFrame& frame);
    size_t capacity() const;
    size_t available() const;
    uint64_t exhaustedCount() const;

private:
    struct State;
    std::shared_ptr<State> m_state;
};

/*
 * C++ 音频图中流动的、拥有压缩数据的音频包。
 *
 * 同样不直接转交 AudioEncoder 回调中的 data 指针，确保 packetHub 的消费者可以异步入队。
 */
struct EncodedAudioPacket {
    AudioCodec codec = AUDIO_CODEC_UNKNOWN;
    std::vector<uint8_t> bytes;
    uint64_t timestampUs = 0;
    uint32_t durationUs = 0;
    AudioPcmFormat sourceFormat {};

    AudioEncodedPacket packetView() const;
    static std::shared_ptr<const EncodedAudioPacket> copyFrom(const AudioEncodedPacket& packet);
};

using EncodedAudioPacketPtr = std::shared_ptr<const EncodedAudioPacket>;

/* 与 AudioFramePool 相同的语义，但 buffer 容纳的是压缩编码包。 */
class EncodedAudioPacketPool {
public:
    EncodedAudioPacketPool(size_t capacity, size_t maximumPacketBytes);

    EncodedAudioPacketPtr copyFrom(const AudioEncodedPacket& packet);
    size_t capacity() const;
    size_t available() const;
    uint64_t exhaustedCount() const;

private:
    struct State;
    std::shared_ptr<State> m_state;
};
