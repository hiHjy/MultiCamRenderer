#include "Stream.hpp"

#include "Log.hpp"
#include "MppDecoder.hpp"
#include "MppTypes.hpp"
#include "RgaEngine.hpp"

#include <condition_variable>
#include <deque>
#include <thread>
#include <utility>
#include <vector>





namespace {

constexpr int kOutputPoolBufferCount = 4;
constexpr size_t kMaxCompressedPackets = 512;

enum class EncodedNaluKind {
    Other,
    ParameterSet,
    RandomAccess,
};

struct EncodedNaluInfo {
    EncodedNaluKind kind = EncodedNaluKind::Other;
    bool hasVps = false;
    bool hasSps = false;
    bool hasPps = false;
};

EncodedNaluInfo inspectAnnexBNalu(VideoCodec codec, const std::vector<uint8_t>& annexB)
{
    size_t offset = 0;
    if (annexB.size() >= 4 && annexB[0] == 0 && annexB[1] == 0 && annexB[2] == 0 && annexB[3] == 1) {
        offset = 4;
    } else if (annexB.size() >= 3 && annexB[0] == 0 && annexB[1] == 0 && annexB[2] == 1) {
        offset = 3;
    } else {
        return {};
    }
    if (offset >= annexB.size()) {
        return {};
    }

    EncodedNaluInfo result;
    if (codec == VideoCodec::H264) {
        const uint8_t nalType = annexB[offset] & 0x1FU;
        if (nalType == 7) {
            result.kind = EncodedNaluKind::ParameterSet;
            result.hasSps = true;
        } else if (nalType == 8) {
            result.kind = EncodedNaluKind::ParameterSet;
            result.hasPps = true;
        } else if (nalType == 5) {
            result.kind = EncodedNaluKind::RandomAccess;
        }
        return result;
    }

    if (codec == VideoCodec::H265 && offset + 1 < annexB.size()) {
        const uint8_t nalType = (annexB[offset] >> 1U) & 0x3FU;
        if (nalType == 32) {
            result.kind = EncodedNaluKind::ParameterSet;
            result.hasVps = true;
        } else if (nalType == 33) {
            result.kind = EncodedNaluKind::ParameterSet;
            result.hasSps = true;
        } else if (nalType == 34) {
            result.kind = EncodedNaluKind::ParameterSet;
            result.hasPps = true;
        } else if (nalType >= 16 && nalType <= 21) {
            // H265 的 BLA/IDR/CRA 都是随机访问点；IPC 发送 IDR 时通常落在 19/20。
            result.kind = EncodedNaluKind::RandomAccess;
        }
    }
    return result;
}

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
        // RTSP PLAY 刚开始收到的首条 NALU 可能是依赖前序参考帧的 P/B 帧。
        // 新 decoder 只能从“参数集 + 随机访问帧”开始，因此首帧也要经过恢复门控。
        enterRecoveryWaitingLocked();
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
            resetRecoveryLocked();
            m_resetDecoderBeforeNextPacket = false;
        }
        m_cv.notify_one();

        if (m_thread.joinable()) {
            m_thread.join();
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        m_running = false;
    }

    void clearPendingPackets()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // 主动丢弃压缩数据后，旧参考链也不再可信；后续必须重新等待恢复边界。
        enterRecoveryWaitingLocked();
    }

    void enqueue(VideoCodec codec, const uint8_t* data, size_t size, uint64_t timestampUs)
    {
        if (data == nullptr || size == 0) {
            return;
        }

        Packet packet;
        packet.codec = codec;
        packet.timestampUs = timestampUs;
        packet.annexB.assign(data, data + size);

        bool shouldNotifyWorker = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_acceptingPackets) {
                return;
            }

            // 压缩 NALU 不能像裸帧一样随便丢弃；丢一条 slice 可能破坏后续参考链。
            if (m_packetQueue.size() >= kMaxCompressedPackets) {
                // 不再继续喂已经断开的参考链：清空积压，并从下一组参数集 + 随机访问帧恢复。
                // 参数集本身不构成图像，不能只等 SPS/PPS 后就恢复送 P 帧。
                enterRecoveryWaitingLocked();
                LOG_WARN("Stream", "streamId=" << m_owner.streamId()
                                                   << " RTSP 压缩 NALU 队列已满，已清空积压并等待下一组参数集 + IDR 恢复");
            }
            // 起流、主动清空或压缩队列严重积压后，都会进入恢复等待状态。
            // 后续 NALU 交给 enqueueRecoveryPacketLocked()：暂存参数集、丢弃普通帧，
            // 直到收齐参数集和随机访问帧。此时 m_waitingForRecovery 重新变为 false，
            // 代表 m_packetQueue 已放入可用于重新开始解码的“参数集 + IDR”；
            // 后续新 NALU 恢复走正常入队路径。
            if (m_waitingForRecovery) {
                shouldNotifyWorker = enqueueRecoveryPacketLocked(std::move(packet));
            } else {
                m_packetQueue.push_back(std::move(packet));
                shouldNotifyWorker = true;
            }
        }
        // 等待恢复期间的普通 NALU 不会入队；只有普通路径入队，或参数集 + IDR
        // 恢复边界完整入队时才需要唤醒 DecodeWorker。
        if (shouldNotifyWorker) {
            m_cv.notify_one();
        }
    }

private:
    struct Packet {
        VideoCodec codec = VideoCodec::H264;
        std::vector<uint8_t> annexB;
        uint64_t timestampUs = 0;
    };

    bool hasCompleteRecoveryParametersLocked() const
    {
        if (m_recoveryCodec == VideoCodec::H264) {
            return m_recoveryHasSps && m_recoveryHasPps;
        }
        if (m_recoveryCodec == VideoCodec::H265) {
            return m_recoveryHasVps && m_recoveryHasSps && m_recoveryHasPps;
        }
        return false;
    }

    void resetRecoveryLocked()
    {
        m_waitingForRecovery = false;
        clearRecoveryParametersLocked();
    }

    // 仅清掉当前等待中的参数集，不改变“是否仍在等待 IDR”的状态。
    void clearRecoveryParametersLocked()
    {
        m_recoveryPackets.clear();
        m_recoveryCodec = VideoCodec::H264;
        m_recoveryParameterSetTimestampUs = 0;
        m_recoveryHasVps = false;
        m_recoveryHasSps = false;
        m_recoveryHasPps = false;
    }

    // 必须在 m_mutex 已加锁时调用。清掉旧码流后，只能由完整恢复边界重新开始：
    // H264 为 SPS + PPS + IDR；H265 为 VPS + SPS + PPS + BLA / IDR / CRA。
    void enterRecoveryWaitingLocked()
    {
        m_packetQueue.clear();
        resetRecoveryLocked();
        m_waitingForRecovery = true;
        m_resetDecoderBeforeNextPacket = false;
    }

	//返回false，不唤醒解码线程
	//返回true， 唤醒解码线程
    bool enqueueRecoveryPacketLocked(Packet packet)
    {
        const EncodedNaluInfo info = inspectAnnexBNalu(packet.codec, packet.annexB);
        if (info.kind == EncodedNaluKind::ParameterSet) {
            // 只保存当前一组参数集。同一个 access unit 的 VPS/SPS/PPS 使用相同
            // timestampUs；时间戳切换意味着上一组没有等到 IDR，旧参数不能再混入
            // 新恢复边界。这样等待期内最多缓存 H264 的 SPS/PPS 或 H265 的 VPS/SPS/PPS。
            if (!m_recoveryPackets.empty()
                && (m_recoveryCodec != packet.codec
                    || m_recoveryParameterSetTimestampUs != packet.timestampUs)) {
                clearRecoveryParametersLocked();
            }
            if (m_recoveryPackets.empty()) {
                m_recoveryCodec = packet.codec;
                m_recoveryParameterSetTimestampUs = packet.timestampUs;
            }
            m_recoveryHasVps = m_recoveryHasVps || info.hasVps;
            m_recoveryHasSps = m_recoveryHasSps || info.hasSps;
            m_recoveryHasPps = m_recoveryHasPps || info.hasPps;
            m_recoveryPackets.push_back(std::move(packet));
            return false;
        }

        if (info.kind != EncodedNaluKind::RandomAccess ||
            packet.codec != m_recoveryCodec || !hasCompleteRecoveryParametersLocked()) {
            // 等待期内普通 P/B NALU、或缺少完整参数集的随机访问帧都不能送入 MPP。
            return false;
        }

        // deinit/init 会在 DecodeWorker 取到这批数据前执行，清掉旧参考帧和 MPP 内部积压。
        m_resetDecoderBeforeNextPacket = true;
        while (!m_recoveryPackets.empty()) {
            m_packetQueue.push_back(std::move(m_recoveryPackets.front()));
            m_recoveryPackets.pop_front();
        }
        m_packetQueue.push_back(std::move(packet));
        resetRecoveryLocked();
        return true;
    }

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

                // 压缩 NALU 队列溢出后，旧参考链已不再可信。
                // 恢复逻辑收齐“参数集 + 随机访问帧”并将它们放入 m_packetQueue 后，
                // 会置 m_resetDecoderBeforeNextPacket = true。DecodeWorker 取到这批恢复
                // 数据前先重置 MPP，随后重新 init 并送入参数集和随机访问帧。
                if (m_resetDecoderBeforeNextPacket) {
                    m_resetDecoderBeforeNextPacket = false;
                    decoder.deinit();
                    decoderInitialized = false;
                }
            }

            const MppCodec packetMppCodec = toMppCodec(packet.codec);
            if (!decoderInitialized || activeCodec != packetMppCodec) {
                decoder.deinit();
                if (!decoder.init(packetMppCodec)) {
                    m_owner.setError("MPP 解码器初始化失败: " + decoder.lastError());
                    continue;
                }
                activeCodec = packetMppCodec;
                decoderInitialized = true;
            }

            CompressedPacket input;
            input.codec = packet.codec;
            input.data = packet.annexB.data();
            input.size = packet.annexB.size();
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
    bool m_waitingForRecovery = false;
    bool m_resetDecoderBeforeNextPacket = false;
    VideoCodec m_recoveryCodec = VideoCodec::H264;
    bool m_recoveryHasVps = false;
    bool m_recoveryHasSps = false;
    bool m_recoveryHasPps = false;
    uint64_t m_recoveryParameterSetTimestampUs = 0;
    std::deque<Packet> m_recoveryPackets;
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

void Stream::onPacket(VideoCodec codec, const uint8_t* data, size_t size, uint64_t timestampUs)
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

void Stream::clearDecodePackets()
{
    if (m_decodeWorker != nullptr) {
        m_decodeWorker->clearPendingPackets();
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
    // readyQueue 满时淘汰旧裸帧的累计数。
    m_readyQueueDroppedFrames = 0;
    // pool 全被 Sink 持有的 lease 占满时，丢弃新裸帧的累计数。
    m_poolExhaustedDroppedFrames = 0;
    // 上次打印 WARN 的时间；重启后发生首次丢帧时可以立即输出统计。
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

void Stream::setRuntimeStateCallback(RuntimeStateCallback callback)
{
    std::lock_guard<std::mutex> lock(m_runtimeStateMutex);
    m_runtimeStateCallback = std::move(callback);
}

void Stream::setRunGeneration(uint64_t generation)
{
    std::lock_guard<std::mutex> lock(m_runtimeStateMutex);
    m_runGeneration = generation;
}

void Stream::reportRuntimeState(RuntimeState state, const std::string& message)
{
    RuntimeStateCallback callback;
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(m_runtimeStateMutex);
        callback = m_runtimeStateCallback;
        generation = m_runGeneration;
    }
    if (callback) {
        callback(generation, state, message);
    }
}
