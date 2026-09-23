#include "AudioFrame.hpp"

#include <cstring>
#include <utility>

namespace {

bool isValidPcmFrame(const AudioPcmFrame& frame)
{
    return frame.data != nullptr && frame.frames != 0 && frame.format.channels != 0
        && frame.format.sampleFormat == AUDIO_SAMPLE_FORMAT_S16_LE;
}

} // namespace

AudioPcmFrame AudioFrame::pcmView() const
{
    AudioPcmFrame frame {};
    frame.data = samples.empty() ? nullptr : samples.data();
    frame.frames = frames;
    frame.format = format;
    frame.timestampUs = timestampUs;
    return frame;
}

std::shared_ptr<const AudioFrame> AudioFrame::copyFrom(const AudioPcmFrame& frame)
{
    if (!isValidPcmFrame(frame)) {
        return nullptr;
    }

    const size_t bytes = frame.frames * frame.format.channels * sizeof(int16_t);
    auto copied = std::make_shared<AudioFrame>();
    copied->samples.resize(bytes);
    std::memcpy(copied->samples.data(), frame.data, bytes);
    copied->format = frame.format;
    copied->frames = frame.frames;
    copied->timestampUs = frame.timestampUs;
    return copied;
}

struct AudioFramePool::State {
    explicit State(size_t capacity, size_t maximumSampleBytes)
        : poolCapacity(capacity)
        , maximumSampleBytes(maximumSampleBytes)
    {
        availableFrames.reserve(capacity);
        for (size_t index = 0; index < capacity; ++index) {
            auto frame = std::make_unique<AudioFrame>();
            frame->samples.resize(maximumSampleBytes);
            availableFrames.push_back(std::move(frame));
        }
    }

    mutable std::mutex mutex;
    std::vector<std::unique_ptr<AudioFrame>> availableFrames;
    size_t poolCapacity = 0;
    size_t maximumSampleBytes = 0;
    uint64_t exhaustedFrames = 0;
};

AudioFramePool::AudioFramePool(size_t capacity, size_t maximumSampleBytes)
    : m_state(std::make_shared<State>(capacity, maximumSampleBytes))
{
}

AudioFramePtr AudioFramePool::copyFrom(const AudioPcmFrame& frame)
{
    if (!isValidPcmFrame(frame)) {
        return nullptr;
    }

    const size_t bytes = frame.frames * frame.format.channels * sizeof(int16_t);
    std::unique_ptr<AudioFrame> copied;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        if (bytes > m_state->maximumSampleBytes || m_state->availableFrames.empty()) {
            ++m_state->exhaustedFrames;
            return nullptr;
        }
        copied = std::move(m_state->availableFrames.back());
        m_state->availableFrames.pop_back();
    }

    /* samples 的 capacity 已在池创建时保证足够；resize 不会再次申请 PCM buffer。 */
    copied->samples.resize(bytes);
    std::memcpy(copied->samples.data(), frame.data, bytes);
    copied->format = frame.format;
    copied->frames = frame.frames;
    copied->timestampUs = frame.timestampUs;

    const std::shared_ptr<State> state = m_state;
    return AudioFramePtr(copied.release(), [state](const AudioFrame* frameToReturn) {
        auto returned = std::unique_ptr<AudioFrame>(const_cast<AudioFrame*>(frameToReturn));
        /* clear 保留预分配 capacity，下一次 copyFrom() 直接复用这块 PCM 内存。 */
        returned->samples.clear();
        returned->format = {};
        returned->frames = 0;
        returned->timestampUs = 0;
        std::lock_guard<std::mutex> lock(state->mutex);
        state->availableFrames.push_back(std::move(returned));
    });
}

size_t AudioFramePool::capacity() const
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->poolCapacity;
}

size_t AudioFramePool::available() const
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->availableFrames.size();
}

uint64_t AudioFramePool::exhaustedCount() const
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->exhaustedFrames;
}

AudioEncodedPacket EncodedAudioPacket::packetView() const
{
    AudioEncodedPacket packet {};
    packet.codec = codec;
    packet.data = bytes.empty() ? nullptr : bytes.data();
    packet.size = bytes.size();
    packet.timestampUs = timestampUs;
    packet.frameSamples = frameSamples;
    packet.durationUs = durationUs;
    packet.sourceFormat = sourceFormat;
    return packet;
}

std::shared_ptr<const EncodedAudioPacket> EncodedAudioPacket::copyFrom(const AudioEncodedPacket& packet)
{
    if (packet.data == nullptr || packet.size == 0 || packet.codec == AUDIO_CODEC_UNKNOWN) {
        return nullptr;
    }

    auto copied = std::make_shared<EncodedAudioPacket>();
    copied->codec = packet.codec;
    copied->bytes.assign(packet.data, packet.data + packet.size);
    copied->timestampUs = packet.timestampUs;
    copied->frameSamples = packet.frameSamples;
    copied->durationUs = packet.durationUs;
    copied->sourceFormat = packet.sourceFormat;
    return copied;
}

struct EncodedAudioPacketPool::State {
    explicit State(size_t capacity, size_t maximumPacketBytes)
        : poolCapacity(capacity)
        , maximumPacketBytes(maximumPacketBytes)
    {
        availablePackets.reserve(capacity);
        for (size_t index = 0; index < capacity; ++index) {
            auto packet = std::make_unique<EncodedAudioPacket>();
            packet->bytes.resize(maximumPacketBytes);
            availablePackets.push_back(std::move(packet));
        }
    }

    mutable std::mutex mutex;
    std::vector<std::unique_ptr<EncodedAudioPacket>> availablePackets;
    size_t poolCapacity = 0;
    size_t maximumPacketBytes = 0;
    uint64_t exhaustedPackets = 0;
};

EncodedAudioPacketPool::EncodedAudioPacketPool(size_t capacity, size_t maximumPacketBytes)
    : m_state(std::make_shared<State>(capacity, maximumPacketBytes))
{
}

EncodedAudioPacketPtr EncodedAudioPacketPool::copyFrom(const AudioEncodedPacket& packet)
{
    if (packet.data == nullptr || packet.size == 0 || packet.codec == AUDIO_CODEC_UNKNOWN) {
        return nullptr;
    }

    std::unique_ptr<EncodedAudioPacket> copied;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        if (packet.size > m_state->maximumPacketBytes || m_state->availablePackets.empty()) {
            ++m_state->exhaustedPackets;
            return nullptr;
        }
        copied = std::move(m_state->availablePackets.back());
        m_state->availablePackets.pop_back();
    }

    copied->bytes.resize(packet.size);
    std::memcpy(copied->bytes.data(), packet.data, packet.size);
    copied->codec = packet.codec;
    copied->timestampUs = packet.timestampUs;
    copied->frameSamples = packet.frameSamples;
    copied->durationUs = packet.durationUs;
    copied->sourceFormat = packet.sourceFormat;

    const std::shared_ptr<State> state = m_state;
    return EncodedAudioPacketPtr(copied.release(), [state](const EncodedAudioPacket* packetToReturn) {
        auto returned = std::unique_ptr<EncodedAudioPacket>(const_cast<EncodedAudioPacket*>(packetToReturn));
        returned->bytes.clear();
        returned->codec = AUDIO_CODEC_UNKNOWN;
        returned->timestampUs = 0;
        returned->frameSamples = 0;
        returned->durationUs = 0;
        returned->sourceFormat = {};
        std::lock_guard<std::mutex> lock(state->mutex);
        state->availablePackets.push_back(std::move(returned));
    });
}

size_t EncodedAudioPacketPool::capacity() const
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->poolCapacity;
}

size_t EncodedAudioPacketPool::available() const
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->availablePackets.size();
}

uint64_t EncodedAudioPacketPool::exhaustedCount() const
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->exhaustedPackets;
}
