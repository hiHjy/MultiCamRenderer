#include "Stream.hpp"

#include "Log.hpp"
#include "MppDecoder.hpp"
#include "RgaEngine.hpp"

#include <condition_variable>
#include <deque>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr int kOutputPoolBufferCount = 4;
constexpr size_t kMaxCompressedPackets = 512;

} // namespace

class Stream::DecodeWorker {
public:
    explicit DecodeWorker(Stream& owner)
        : m_owner(owner)
    {
    }

    ~DecodeWorker()
    {
        stop();
    }

    bool start()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_running) {
            return true;
        }

        m_stopRequested = false;
        m_acceptingPackets = true;
        m_running = true;
        m_thread = std::thread(&DecodeWorker::workLoop, this);
        return true;
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_acceptingPackets = false;
            m_stopRequested = true;
            m_packetQueue.clear();
        }
        m_cv.notify_one();

        if (m_thread.joinable()) {
            m_thread.join();
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        m_running = false;
    }

    void enqueue(MppCodec codec, const uint8_t* data, size_t size, uint64_t timestampUs)
    {
        if (data == nullptr || size == 0) {
            return;
        }

        Packet packet;
        packet.codec = codec;
        packet.timestampUs = timestampUs;
        packet.annexB.assign(data, data + size);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_acceptingPackets) {
                return;
            }

            // 压缩 NALU 不能像裸帧一样随便丢弃；丢一条 slice 可能破坏后续参考链。
            // 当前先明确报错，后续接编码器 IDR 请求后再实现“等待关键帧恢复”。
            if (m_packetQueue.size() >= kMaxCompressedPackets) {
                m_owner.setError("RTSP 压缩 NALU 队列已满，NALU 已丢弃，需等待 IDR 恢复");
                return;
            }

            m_packetQueue.push_back(std::move(packet));
        }
        m_cv.notify_one();
    }

private:
    struct Packet {
        MppCodec codec = MppCodec::H264;
        std::vector<uint8_t> annexB;
        uint64_t timestampUs = 0;
    };

    void workLoop()
    {
        MppDecoder decoder;
        RgaEngine rga;
        MppCodec activeCodec = MppCodec::H264;
        bool decoderInitialized = false;

        decoder.setFrameCallback([this, &rga](const VideoFrame& decodedFrame) {
            FramePacket stablePacket;
            if (!m_owner.makeStableFramePacket(decodedFrame, rga, stablePacket)) {
                // pool 暂时全被下游 lease 持有时，丢当前裸帧是安全的；不能让 MPP 停住。
                return true;
            }
            m_owner.enqueueDecodedFrame(std::move(stablePacket));
            return true;
        });

        while (true) {
            Packet packet;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] {
                    return m_stopRequested || !m_packetQueue.empty();
                });

                if (m_stopRequested) {
                    break;
                }

                packet = std::move(m_packetQueue.front());
                m_packetQueue.pop_front();
            }

            if (!decoderInitialized || activeCodec != packet.codec) {
                decoder.deinit();
                if (!decoder.init(packet.codec)) {
                    m_owner.setError("MPP 解码器初始化失败: " + decoder.lastError());
                    continue;
                }
                activeCodec = packet.codec;
                decoderInitialized = true;
            }

            VideoFrame input;
            input.va = packet.annexB.data();
            input.capacity = packet.annexB.size();
            input.bytesUsed = packet.annexB.size();
            input.timestampUs = packet.timestampUs;

            if (!decoder.sendPacket(input)) {
                m_owner.setError("MPP 解码失败: " + decoder.lastError());
            }
        }

        decoder.deinit();
    }

private:
    Stream& m_owner;
    std::thread m_thread;
    std::deque<Packet> m_packetQueue;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_running = false;
    bool m_acceptingPackets = false;
    bool m_stopRequested = false;
};

Stream::Stream(size_t readyQueueCapacity)
    : m_readyQueueCapacity(readyQueueCapacity == 0 ? 1 : readyQueueCapacity)
{
}

Stream::~Stream()
{
    stopDecodeWorker();
    clearReadyFrames();
    m_activePool.reset();
}

bool Stream::OutputLayout::matches(const VideoFrame& frame) const
{
    return width == frame.width && height == frame.height &&
           stride == videoFrameEffectiveStride(frame) &&
           heightStride == videoFrameEffectiveHeightStride(frame) &&
           format == frame.format;
}

void Stream::onPacket(MppCodec codec, const uint8_t* data, size_t size, uint64_t timestampUs)
{
    if (m_decodeWorker != nullptr) {
        m_decodeWorker->enqueue(codec, data, size, timestampUs);
    }
}

bool Stream::startDecodeWorker()
{
    if (m_decodeWorker == nullptr) {
        m_decodeWorker = std::make_unique<DecodeWorker>(*this);
    }
    m_outputSequence = 0;
    resetDroppedFrameStatistics();
    return m_decodeWorker->start();
}

void Stream::stopDecodeWorker()
{
    if (m_decodeWorker != nullptr) {
        m_decodeWorker->stop();
        // 节流期间的最后几次丢帧也要在 stop 时汇总出来，保证日志累计数准确。
        logDroppedFrameStatistics(true);
        m_decodeWorker.reset();
    }
}

bool Stream::ensureOutputPool(const VideoFrame& decodedFrame)
{
    if (decodedFrame.format != PixelFormat::NV12 || decodedFrame.width <= 0 || decodedFrame.height <= 0) {
        setError("MPP 输出不是有效 NV12 帧");
        return false;
    }

    if (m_activePool != nullptr && m_outputLayout.matches(decodedFrame)) {
        return true;
    }

    const int stride = videoFrameEffectiveStride(decodedFrame);
    const int heightStride = videoFrameEffectiveHeightStride(decodedFrame);
    const size_t payloadSize = videoFrameBufferSizeFor(PixelFormat::NV12,
                                                        stride,
                                                        heightStride,
                                                        VideoBufferSizeMode::Payload);
    if (payloadSize == 0) {
        setError("MPP 输出 layout 无效，无法创建稳定输出池");
        return false;
    }

    auto nextPool = std::make_shared<DmaBufferPool>();
    if (!nextPool->init(kOutputPoolBufferCount, payloadSize)) {
        setError("创建稳定输出 DMA pool 失败: " + nextPool->lastError());
        return false;
    }

    // 新 layout 已经准备好，再丢弃未发布的旧分辨率帧；已交给下游的旧帧由其 lease 持有旧 pool。
    clearReadyFrames();
    m_activePool = std::move(nextPool);
    m_outputLayout = {
        decodedFrame.width,
        decodedFrame.height,
        stride,
        heightStride,
        decodedFrame.format,
    };
    return true;
}

bool Stream::makeStableFramePacket(const VideoFrame& decodedFrame,
                                   RgaEngine& rga,
                                   FramePacket& packet)
{
    if (!ensureOutputPool(decodedFrame)) {
        return false;
    }

    const std::shared_ptr<DmaBufferPool> pool = m_activePool;
    VideoFrame* outputFrame = pool->acquireFrame();
    if (outputFrame == nullptr) {
        // readyQueue 中的帧还未交给下游，可以淘汰并立即归还该 buffer。
        if (discardPendingReadyFrame()) {
            recordDroppedFrames(1, 0);
            outputFrame = pool->acquireFrame();
        }
        if (outputFrame == nullptr) {
            // 剩余 buffer 已被 Hub/Sink 持有，不能抢回；当前裸帧直接丢弃以维持低延迟。
            recordDroppedFrames(0, 1);
            return false;
        }
    }

    outputFrame->width = decodedFrame.width;
    outputFrame->height = decodedFrame.height;
    outputFrame->stride = videoFrameEffectiveStride(decodedFrame);
    outputFrame->heightStride = videoFrameEffectiveHeightStride(decodedFrame);
    outputFrame->format = PixelFormat::NV12;
    outputFrame->nativeFormat = decodedFrame.nativeFormat;
    outputFrame->timestampUs = decodedFrame.timestampUs;
    outputFrame->sequence = decodedFrame.sequence;

    if (!rga.copy(decodedFrame, *outputFrame)) {
        setError("RGA 复制 MPP 临时帧失败: " + rga.lastError());
        (void)pool->releaseFrame(outputFrame);
        return false;
    }

    // RGA 会复制 MPP 临时帧的元数据；streamId/sequence 属于 Stream 自己的稳定输出，
    // 需要在 copy 完成后覆盖。
    outputFrame->streamId = m_streamId;
    outputFrame->sequence = ++m_outputSequence;
    packet.frame = *outputFrame;
    packet.lease = std::make_shared<FrameLease>([pool, outputFrame] {
        (void)pool->releaseFrame(outputFrame);
    });
    return true;
}

bool Stream::discardPendingReadyFrame()
{
    std::lock_guard<std::mutex> lock(m_readyMutex);
    if (m_readyQueue.empty()) {
        return false;
    }

    // 只释放一块 buffer 即可让当前新帧重试 acquire；容量大于 1 时不能把整队列都清掉。
    m_readyQueue.pop_front();
    return true;
}

void Stream::clearReadyFrames()
{
    std::lock_guard<std::mutex> lock(m_readyMutex);
    m_readyQueue.clear();
}

void Stream::enqueueDecodedFrame(FramePacket packet)
{
    std::function<void()> frameReadyCallback;
    size_t droppedFrames = 0;
    {
        std::lock_guard<std::mutex> lock(m_readyMutex);
        while (m_readyQueue.size() >= m_readyQueueCapacity) {
            m_readyQueue.pop_front();
            ++droppedFrames;
        }
        m_readyQueue.push_back(std::move(packet));
        frameReadyCallback = m_frameReadyCallback;
    }

    if (droppedFrames != 0) {
        recordDroppedFrames(droppedFrames, 0);
    }

    if (frameReadyCallback) {
        frameReadyCallback();
    }
}

void Stream::recordDroppedFrames(size_t readyQueueFrames, size_t poolExhaustedFrames)
{
    if (readyQueueFrames == 0 && poolExhaustedFrames == 0) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_dropStatisticsMutex);
        m_readyQueueDroppedFrames += readyQueueFrames;
        m_poolExhaustedDroppedFrames += poolExhaustedFrames;
    }

    logDroppedFrameStatistics(false);
}

void Stream::logDroppedFrameStatistics(bool force)
{
    uint64_t readyQueueDroppedFrames = 0;
    uint64_t poolExhaustedDroppedFrames = 0;
    {
        std::lock_guard<std::mutex> lock(m_dropStatisticsMutex);
        if (m_readyQueueDroppedFrames == 0 && m_poolExhaustedDroppedFrames == 0) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (!force && m_lastDropStatisticsLog.time_since_epoch().count() != 0 &&
            now - m_lastDropStatisticsLog < std::chrono::seconds(1)) {
            return;
        }
        m_lastDropStatisticsLog = now;
        readyQueueDroppedFrames = m_readyQueueDroppedFrames;
        poolExhaustedDroppedFrames = m_poolExhaustedDroppedFrames;
    }

    LOG_WARN("Stream", "streamId=" << m_streamId
                                     << " 稳定裸帧丢弃累计="
                                     << (readyQueueDroppedFrames + poolExhaustedDroppedFrames)
                                     << " [readyQueue淘汰=" << readyQueueDroppedFrames
                                     << ", pool被下游lease占满=" << poolExhaustedDroppedFrames
                                     << "] readyQueueCapacity=" << m_readyQueueCapacity);
}

void Stream::resetDroppedFrameStatistics()
{
    std::lock_guard<std::mutex> lock(m_dropStatisticsMutex);
    m_readyQueueDroppedFrames = 0;
    m_poolExhaustedDroppedFrames = 0;
    m_lastDropStatisticsLog = {};
}

bool Stream::tryGetFrame(FramePacket& packet)
{
    std::lock_guard<std::mutex> lock(m_readyMutex);
    if (m_readyQueue.empty()) {
        return false;
    }

    packet = std::move(m_readyQueue.front());
    m_readyQueue.pop_front();
    return true;
}

int Stream::streamId() const
{
    return m_streamId;
}

std::string Stream::lastError() const
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_lastError;
}

void Stream::setError(const std::string& message)
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    m_lastError = message;
}

void Stream::clearError()
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    m_lastError.clear();
}

void Stream::setStreamId(int streamId)
{
    m_streamId = streamId;
}

void Stream::setFrameReadyCallback(std::function<void()> callback)
{
    std::lock_guard<std::mutex> lock(m_readyMutex);
    m_frameReadyCallback = std::move(callback);
}
