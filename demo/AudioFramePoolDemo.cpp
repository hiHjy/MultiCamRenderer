#include "AudioFrame.hpp"
#include "Log.hpp"

#include <array>
#include <cstdint>

namespace {

AudioPcmFormat makeMonoS16Format()
{
    AudioPcmFormat format {};
    format.sampleRate = 48000;
    format.channels = 1;
    format.sampleFormat = AUDIO_SAMPLE_FORMAT_S16_LE;
    return format;
}

} // namespace

/*
 * 不依赖 ALSA 的小测试：验证 AudioFramePool / EncodedAudioPacketPool 的三个不变式：
 *
 * 1. 池内对象借出时不会被重复使用；
 * 2. 池满立刻返回 nullptr，不会阻塞；
 * 3. 最后一个 shared_ptr 释放后，对象回到池中并复用原有 payload buffer。
 */
int main()
{
    constexpr size_t kPcmFrames = 480; // 48kHz 下 10ms
    constexpr size_t kPcmBytes = kPcmFrames * sizeof(int16_t);
    std::array<uint8_t, kPcmBytes> pcmBytes {};
    const AudioPcmFormat pcmFormat = makeMonoS16Format();
    const AudioPcmFrame pcm { pcmBytes.data(), kPcmFrames, pcmFormat, 123456 };

    AudioFramePool pcmPool(2, kPcmBytes);
    AudioFramePtr pcmFirst = pcmPool.copyFrom(pcm);
    AudioFramePtr pcmSecond = pcmPool.copyFrom(pcm);
    const AudioFramePtr pcmOverflow = pcmPool.copyFrom(pcm);
    if (!pcmFirst || !pcmSecond || pcmOverflow || pcmPool.available() != 0
        || pcmPool.exhaustedCount() != 1) {
        LOG_ERROR("AudioFramePoolDemo", "PCM 池满行为不符合预期");
        return 1;
    }

    const uint8_t* const firstPayloadAddress = pcmFirst->samples.data();
    pcmFirst.reset();
    AudioFramePtr pcmReused = pcmPool.copyFrom(pcm);
    if (!pcmReused || pcmReused->samples.data() != firstPayloadAddress) {
        LOG_ERROR("AudioFramePoolDemo", "PCM 对象没有复用原有 payload buffer");
        return 1;
    }
    pcmSecond.reset();
    pcmReused.reset();
    if (pcmPool.available() != pcmPool.capacity()) {
        LOG_ERROR("AudioFramePoolDemo", "PCM 帧未全部归还");
        return 1;
    }

    std::array<uint8_t, 64> opusBytes {};
    const AudioEncodedPacket opus {
        AUDIO_CODEC_OPUS, opusBytes.data(), opusBytes.size(), 123456, 20000, pcmFormat
    };
    EncodedAudioPacketPool packetPool(2, 128);
    EncodedAudioPacketPtr packetFirst = packetPool.copyFrom(opus);
    EncodedAudioPacketPtr packetSecond = packetPool.copyFrom(opus);
    const EncodedAudioPacketPtr packetOverflow = packetPool.copyFrom(opus);
    if (!packetFirst || !packetSecond || packetOverflow || packetPool.available() != 0
        || packetPool.exhaustedCount() != 1) {
        LOG_ERROR("AudioFramePoolDemo", "压缩包池满行为不符合预期");
        return 1;
    }

    const uint8_t* const firstPacketAddress = packetFirst->bytes.data();
    packetFirst.reset();
    EncodedAudioPacketPtr packetReused = packetPool.copyFrom(opus);
    if (!packetReused || packetReused->bytes.data() != firstPacketAddress) {
        LOG_ERROR("AudioFramePoolDemo", "压缩包对象没有复用原有 payload buffer");
        return 1;
    }
    packetSecond.reset();
    packetReused.reset();
    if (packetPool.available() != packetPool.capacity()) {
        LOG_ERROR("AudioFramePoolDemo", "压缩包未全部归还");
        return 1;
    }

    LOG_INFO("AudioFramePoolDemo", "PASS PCM/packet 池均完成满池丢弃与 buffer 复用验证");
    return 0;
}
