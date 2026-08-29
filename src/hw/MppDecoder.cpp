#include "hw/MppDecoder.hpp"

#include "DmaAllocator.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <linux/dma-buf.h>
#include <mpp_buffer.h>
#include <mpp_frame.h>
#include <mpp_packet.h>
#include <mpp_task.h>
#include <rk_mpi.h>
#include <rk_vdec_cfg.h>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr int kStreamOutputBufferCount = 32;
constexpr size_t kDefaultPacketBufferSize = 4 * 1024 * 1024;

int alignUp(int value, int alignment)
{
    if (alignment <= 1)
        return value;
    return ((value + alignment - 1) / alignment) * alignment;
}

MppCodingType toMppCoding(MppCodec codec)
{
    switch (codec) {
    case MppCodec::MJPEG:
        return MPP_VIDEO_CodingMJPEG;
    case MppCodec::H264:
        return MPP_VIDEO_CodingAVC;
    case MppCodec::H265:
        return MPP_VIDEO_CodingHEVC;
    }
    return MPP_VIDEO_CodingUnused;
}

PixelFormat fromMppFrameFormat(RK_U32 format)
{
    switch (format & MPP_FRAME_FMT_MASK) {
    case MPP_FMT_YUV420SP:
        return PixelFormat::NV12;
    case MPP_FMT_YUV420P:
        return PixelFormat::YUV420P;
    case MPP_FMT_YUV422_YUYV:
        return PixelFormat::YUYV;
    case MPP_FMT_RGBA8888:
        return PixelFormat::RGBA8888;
    default:
        return PixelFormat::Unknown;
    }
}

size_t mppOutputBufferSize(int stride, int heightStride)
{
    if (stride <= 0 || heightStride <= 0)
        return 0;
    // MPP decoder external buffers need YUV data plus extra metadata space.
    // Rockchip's guide recommends hor_stride * ver_stride * 2 for YUV420.
    return static_cast<size_t>(stride) * static_cast<size_t>(heightStride) * 2;
}

void dmabufSync(int fd, unsigned long flags)
{
    if (fd < 0)
        return;

    dma_buf_sync sync {};
    sync.flags = flags;
    (void)ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

bool isJpegSofMarker(unsigned char marker)
{
    switch (marker) {
    case 0xC0:
    case 0xC1:
    case 0xC2:
    case 0xC3:
    case 0xC5:
    case 0xC6:
    case 0xC7:
    case 0xC9:
    case 0xCA:
    case 0xCB:
    case 0xCD:
    case 0xCE:
    case 0xCF:
        return true;
    default:
        return false;
    }
}

bool parseJpegSize(const unsigned char* data, size_t len, int& width, int& height)
{
    if (data == nullptr || len < 4 || data[0] != 0xFF || data[1] != 0xD8)
        return false;

    size_t pos = 2;
    while (pos + 3 < len) {
        while (pos < len && data[pos] != 0xFF) {
            ++pos;
        }
        while (pos < len && data[pos] == 0xFF) {
            ++pos;
        }
        if (pos >= len)
            break;

        const unsigned char marker = data[pos++];
        if (marker == 0xD8 || marker == 0x01)
            continue;
        if (marker == 0xD9 || marker == 0xDA)
            break;
        if (pos + 1 >= len)
            break;

        const size_t segLen = (static_cast<size_t>(data[pos]) << 8) | data[pos + 1];
        pos += 2;
        if (segLen < 2 || pos + segLen - 2 > len)
            break;

        if (isJpegSofMarker(marker)) {
            if (segLen < 7)
                return false;
            height = (static_cast<int>(data[pos + 1]) << 8) | data[pos + 2];
            width = (static_cast<int>(data[pos + 3]) << 8) | data[pos + 4];
            return width > 0 && height > 0;
        }

        pos += segLen - 2;
    }

    return false;
}

std::string mppError(const char* prefix, MPP_RET ret)
{
    std::ostringstream oss;
    oss << prefix << " failed ret=" << ret;
    return oss.str();
}

} // namespace

struct MppDecoder::Impl {
    using FrameCallback = MppDecoder::FrameCallback;

    Impl();
    ~Impl();

    bool init(MppCodec codec);
    void deinit();
    bool decodeMjpeg(const VideoFrame& input, VideoFrame& output);
    bool sendPacket(const VideoFrame& packet, bool eos);
    void setFrameCallback(FrameCallback callback);
    const std::string& lastError() const;

private:
    bool initInternal(MppCodec codec);
    bool decodeMjpegInternal(const VideoFrame& input, VideoFrame& output);
    bool sendPacketInternal(const VideoFrame& packet, bool eos);
    int pollFrames();
    bool handleStreamFrame(MppFrame frame);
    bool handleInfoChange(MppFrame frame);
    void setError(const std::string& message);

private:
    MppCodec m_codec = MppCodec::MJPEG;
    bool m_initialized = false;
    MppCtx m_ctx = nullptr;
    MppApi* m_mpi = nullptr;
    MppPacket m_streamPacket = nullptr;
    MppDecCfg m_decCfg = nullptr;
    MppBufferGroup m_frameGroup = nullptr;
    std::vector<unsigned char> m_packetBuffer;
    std::vector<DmaMemory> m_streamBuffers;
    FrameCallback m_callback;
    std::string m_lastError;
};

MppDecoder::MppDecoder()
    : m_impl(std::make_unique<Impl>())
{
}

MppDecoder::~MppDecoder() = default;

bool MppDecoder::init(MppCodec codec)
{
    return m_impl->init(codec);
}

void MppDecoder::deinit()
{
    m_impl->deinit();
}

bool MppDecoder::decodeMjpeg(const VideoFrame& input, VideoFrame& output)
{
    return m_impl->decodeMjpeg(input, output);
}

bool MppDecoder::sendPacket(const VideoFrame& packet, bool eos)
{
    return m_impl->sendPacket(packet, eos);
}

void MppDecoder::setFrameCallback(FrameCallback callback)
{
    m_impl->setFrameCallback(std::move(callback));
}

const std::string& MppDecoder::lastError() const
{
    return m_impl->lastError();
}

MppDecoder::Impl::Impl()
    : m_packetBuffer(kDefaultPacketBufferSize)
{
}

MppDecoder::Impl::~Impl()
{
    deinit();
}

bool MppDecoder::Impl::init(MppCodec codec)
{
    if (!initInternal(codec))
        return false;

    m_lastError.clear();
    return true;
}

void MppDecoder::Impl::deinit()
{
    m_streamBuffers.clear();

    if (m_frameGroup != nullptr) {
        mpp_buffer_group_put(m_frameGroup);
        m_frameGroup = nullptr;
    }

    if (m_streamPacket != nullptr) {
        mpp_packet_deinit(&m_streamPacket);
        m_streamPacket = nullptr;
    }

    if (m_decCfg != nullptr) {
        mpp_dec_cfg_deinit(m_decCfg);
        m_decCfg = nullptr;
    }

    if (m_ctx != nullptr) {
        if (m_mpi != nullptr) {
            (void)m_mpi->reset(m_ctx);
        }
        mpp_destroy(m_ctx);
        m_ctx = nullptr;
        m_mpi = nullptr;
    }

    m_initialized = false;
}

bool MppDecoder::Impl::decodeMjpeg(const VideoFrame& input, VideoFrame& output)
{
    if (!decodeMjpegInternal(input, output))
        return false;

    m_lastError.clear();
    return true;
}

bool MppDecoder::Impl::sendPacket(const VideoFrame& packet, bool eos)
{
    if (!sendPacketInternal(packet, eos))
        return false;

    m_lastError.clear();
    return true;
}

void MppDecoder::Impl::setFrameCallback(FrameCallback callback)
{
    m_callback = std::move(callback);
}

const std::string& MppDecoder::Impl::lastError() const
{
    return m_lastError;
}

bool MppDecoder::Impl::initInternal(MppCodec codec)
{
    if (m_initialized && m_codec == codec)
        return true;

    deinit();
    m_codec = codec;

    const MppCodingType coding = toMppCoding(codec);
    if (coding == MPP_VIDEO_CodingUnused) {
        setError("不支持的 MPP codec");
        return false;
    }

    MPP_RET ret = mpp_create(&m_ctx, &m_mpi);
    if (ret != MPP_OK) {
        setError(mppError("mpp_create", ret));
        deinit();
        return false;
    }

    if (codec == MppCodec::H264 || codec == MppCodec::H265) {
        RK_U32 needSplit = 1;
        ret = m_mpi->control(m_ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, &needSplit);
        if (ret != MPP_OK) {
            setError(mppError("MPP_DEC_SET_PARSER_SPLIT_MODE", ret));
            deinit();
            return false;
        }
    }

    ret = mpp_init(m_ctx, MPP_CTX_DEC, coding);
    if (ret != MPP_OK) {
        setError(mppError("mpp_init decoder", ret));
        deinit();
        return false;
    }

    ret = mpp_dec_cfg_init(&m_decCfg);
    if (ret != MPP_OK) {
        setError(mppError("mpp_dec_cfg_init", ret));
        deinit();
        return false;
    }

    ret = m_mpi->control(m_ctx, MPP_DEC_GET_CFG, m_decCfg);
    if (ret != MPP_OK) {
        setError(mppError("MPP_DEC_GET_CFG", ret));
        deinit();
        return false;
    }

    const RK_U32 splitParse = codec == MppCodec::MJPEG ? 0 : 1;
    ret = mpp_dec_cfg_set_u32(m_decCfg, "base:split_parse", splitParse);
    if (ret != MPP_OK) {
        setError(mppError("mpp_dec_cfg_set_u32(base:split_parse)", ret));
        deinit();
        return false;
    }

    ret = m_mpi->control(m_ctx, MPP_DEC_SET_CFG, m_decCfg);
    if (ret != MPP_OK) {
        setError(mppError("MPP_DEC_SET_CFG", ret));
        deinit();
        return false;
    }

    if (codec == MppCodec::MJPEG) {
        RK_U32 outputFormat = MPP_FMT_YUV420SP;
        (void)m_mpi->control(m_ctx, MPP_DEC_SET_OUTPUT_FORMAT, &outputFormat);
    } else {
        ret = mpp_packet_init(&m_streamPacket, nullptr, 0);
        if (ret != MPP_OK) {
            setError(mppError("mpp_packet_init", ret));
            deinit();
            return false;
        }
    }

    m_initialized = true;
    return true;
}

bool MppDecoder::Impl::decodeMjpegInternal(const VideoFrame& input, VideoFrame& output)
{
    if (input.format != PixelFormat::MJPEG) {
        setError("decodeMjpeg 只接受 MJPEG 输入");
        return false;
    }
    if (input.dmaFd < 0 || input.bytesUsed == 0) {
        setError("MJPEG 输入 dmaFd/bytesUsed 无效");
        return false;
    }

    int width = input.width;
    int height = input.height;
    dmabufSync(input.dmaFd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
    void* mapped = mmap(nullptr, input.bytesUsed, PROT_READ, MAP_SHARED, input.dmaFd, 0);
    if (mapped != MAP_FAILED) {
        (void)parseJpegSize(static_cast<const unsigned char*>(mapped), input.bytesUsed, width, height);
        munmap(mapped, input.bytesUsed);
    }
    dmabufSync(input.dmaFd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);

    if (width <= 0 || height <= 0) {
        setError("MJPEG 宽高无效，既不能解析 JPEG 头，也没有 input 宽高兜底");
        return false;
    }

    const int stride = alignUp(width, 16);
    const int heightStride = alignUp(height, 16);
    const size_t outputSize = mppOutputBufferSize(stride, heightStride);
    if (output.dmaFd < 0 || output.capacity < outputSize) {
        std::ostringstream oss;
        oss << "MJPEG 输出 buffer 无效或容量不足: fd=" << output.dmaFd
            << " capacity=" << output.capacity
            << " required=" << outputSize;
        setError(oss.str());
        return false;
    }

    if (!initInternal(MppCodec::MJPEG))
        return false;

    MppPacket packet = nullptr;
    MppPacket packetOut = nullptr;
    MppFrame frame = nullptr;
    MppFrame frameOut = nullptr;
    MppTask task = nullptr;
    MppBuffer inBuf = nullptr;
    MppBuffer outBuf = nullptr;
    MppBufferInfo inInfo {};
    MppBufferInfo outInfo {};
    bool inputSubmitted = false;
    bool inputSyncStarted = false;
    bool outputSyncStarted = false;
    MPP_RET ret = MPP_OK;

    dmabufSync(input.dmaFd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
    inputSyncStarted = true;
    dmabufSync(output.dmaFd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    outputSyncStarted = true;

    inInfo.type = MPP_BUFFER_TYPE_EXT_DMA;
    inInfo.fd = input.dmaFd;
    inInfo.size = input.bytesUsed;
    ret = mpp_buffer_import(&inBuf, &inInfo);
    if (ret != MPP_OK) {
        setError(mppError("mpp_buffer_import(input)", ret));
        goto out;
    }

    ret = mpp_packet_init_with_buffer(&packet, inBuf);
    if (ret != MPP_OK) {
        setError(mppError("mpp_packet_init_with_buffer", ret));
        goto out;
    }

    void* packetData;
    packetData = mpp_packet_get_data(packet);
    mpp_packet_set_data(packet, packetData);
    mpp_packet_set_pos(packet, packetData);
    mpp_packet_set_size(packet, input.bytesUsed);
    mpp_packet_set_length(packet, input.bytesUsed);
    mpp_packet_set_pts(packet, static_cast<RK_S64>(input.timestampUs));
    mpp_packet_set_dts(packet, static_cast<RK_S64>(input.timestampUs));
    mpp_packet_clr_eos(packet);

    outInfo.type = MPP_BUFFER_TYPE_EXT_DMA;
    outInfo.fd = output.dmaFd;
    outInfo.size = outputSize;
    ret = mpp_buffer_import(&outBuf, &outInfo);
    if (ret != MPP_OK) {
        setError(mppError("mpp_buffer_import(output)", ret));
        goto out;
    }

    ret = mpp_frame_init(&frame);
    if (ret != MPP_OK) {
        setError(mppError("mpp_frame_init", ret));
        goto out;
    }

    mpp_frame_set_buffer(frame, outBuf);
    mpp_frame_set_width(frame, static_cast<RK_U32>(width));
    mpp_frame_set_height(frame, static_cast<RK_U32>(height));
    mpp_frame_set_hor_stride(frame, static_cast<RK_U32>(stride));
    mpp_frame_set_ver_stride(frame, static_cast<RK_U32>(heightStride));
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_buf_size(frame, outputSize);

    ret = m_mpi->poll(m_ctx, MPP_PORT_INPUT, MPP_POLL_BLOCK);
    if (ret != MPP_OK) {
        setError(mppError("MPP_PORT_INPUT poll", ret));
        goto out;
    }

    ret = m_mpi->dequeue(m_ctx, MPP_PORT_INPUT, &task);
    if (ret != MPP_OK || task == nullptr) {
        setError(mppError("MPP_PORT_INPUT dequeue", ret));
        goto out;
    }

    mpp_task_meta_set_packet(task, KEY_INPUT_PACKET, packet);
    mpp_task_meta_set_frame(task, KEY_OUTPUT_FRAME, frame);

    ret = m_mpi->enqueue(m_ctx, MPP_PORT_INPUT, task);
    if (ret != MPP_OK) {
        setError(mppError("MPP_PORT_INPUT enqueue", ret));
        goto out;
    }
    task = nullptr;
    inputSubmitted = true;

    ret = m_mpi->poll(m_ctx, MPP_PORT_OUTPUT, MPP_POLL_BLOCK);
    if (ret != MPP_OK) {
        setError(mppError("MPP_PORT_OUTPUT poll", ret));
        goto out;
    }

    ret = m_mpi->dequeue(m_ctx, MPP_PORT_OUTPUT, &task);
    if (ret != MPP_OK || task == nullptr) {
        setError(mppError("MPP_PORT_OUTPUT dequeue", ret));
        goto out;
    }

    ret = mpp_task_meta_get_frame(task, KEY_OUTPUT_FRAME, &frameOut);
    if (ret != MPP_OK || frameOut == nullptr) {
        setError(mppError("mpp_task_meta_get_frame", ret));
        goto out;
    }

    if (outputSyncStarted) {
        dmabufSync(output.dmaFd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
        outputSyncStarted = false;
    }

    if (mpp_frame_get_errinfo(frameOut) != 0 || mpp_frame_get_discard(frameOut) != 0) {
        setError("MPP MJPEG 解码输出帧标记为错误或丢弃");
        ret = MPP_NOK;
        goto out;
    }

    output.width = static_cast<int>(mpp_frame_get_width(frameOut));
    output.height = static_cast<int>(mpp_frame_get_height(frameOut));
    output.stride = static_cast<int>(mpp_frame_get_hor_stride(frameOut));
    output.heightStride = static_cast<int>(mpp_frame_get_ver_stride(frameOut));
    output.format = PixelFormat::NV12;
    output.nativeFormat = static_cast<uint32_t>(mpp_frame_get_fmt(frameOut));
    output.bytesUsed = mpp_frame_get_buf_size(frameOut);
    output.timestampUs = input.timestampUs;
    output.sequence = input.sequence;

    ret = m_mpi->enqueue(m_ctx, MPP_PORT_OUTPUT, task);
    if (ret != MPP_OK) {
        setError(mppError("MPP_PORT_OUTPUT recycle enqueue", ret));
        goto out;
    }
    task = nullptr;

    ret = m_mpi->dequeue(m_ctx, MPP_PORT_INPUT, &task);
    if (ret != MPP_OK || task == nullptr) {
        setError(mppError("MPP_PORT_INPUT recycle dequeue", ret));
        goto out;
    }

    ret = mpp_task_meta_get_packet(task, KEY_INPUT_PACKET, &packetOut);
    if (ret == MPP_OK && packetOut != nullptr) {
        mpp_packet_deinit(&packetOut);
        packet = nullptr;
    }

    ret = m_mpi->enqueue(m_ctx, MPP_PORT_INPUT, task);
    if (ret != MPP_OK) {
        setError(mppError("MPP_PORT_INPUT recycle enqueue", ret));
        goto out;
    }
    task = nullptr;

    if (inputSyncStarted) {
        dmabufSync(input.dmaFd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        inputSyncStarted = false;
    }

    ret = MPP_OK;

out:
    if (task != nullptr) {
        if (inputSubmitted) {
            (void)m_mpi->enqueue(m_ctx, MPP_PORT_OUTPUT, task);
        } else {
            (void)m_mpi->enqueue(m_ctx, MPP_PORT_INPUT, task);
        }
    }
    if (packet != nullptr)
        mpp_packet_deinit(&packet);
    if (frame != nullptr)
        mpp_frame_deinit(&frame);
    if (outBuf != nullptr)
        mpp_buffer_put(outBuf);
    if (inBuf != nullptr)
        mpp_buffer_put(inBuf);
    if (outputSyncStarted)
        dmabufSync(output.dmaFd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    if (inputSyncStarted)
        dmabufSync(input.dmaFd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);

    return ret == MPP_OK;
}

bool MppDecoder::Impl::sendPacketInternal(const VideoFrame& packet, bool eos)
{
    if (m_codec != MppCodec::H264 && m_codec != MppCodec::H265) {
        setError("sendPacket 只适用于 H264/H265");
        return false;
    }
    if (!initInternal(m_codec))
        return false;
    if (packet.bytesUsed > 0 && packet.va == nullptr) {
        setError("H264/H265 packet 需要有效 va");
        return false;
    }

    if (m_packetBuffer.size() < packet.bytesUsed) {
        m_packetBuffer.resize(std::max(packet.bytesUsed, kDefaultPacketBufferSize));
    }
    if (packet.bytesUsed > 0) {
        std::memcpy(m_packetBuffer.data(), packet.va, packet.bytesUsed);
    }

    mpp_packet_set_data(m_streamPacket, m_packetBuffer.data());
    mpp_packet_set_pos(m_streamPacket, m_packetBuffer.data());
    mpp_packet_set_size(m_streamPacket, packet.bytesUsed);
    mpp_packet_set_length(m_streamPacket, packet.bytesUsed);
    mpp_packet_set_pts(m_streamPacket, static_cast<RK_S64>(packet.timestampUs));
    mpp_packet_set_dts(m_streamPacket, static_cast<RK_S64>(packet.timestampUs));
    mpp_packet_clr_eos(m_streamPacket);
    if (eos)
        mpp_packet_set_eos(m_streamPacket);

    bool packetDone = false;
    while (!packetDone) {
        const MPP_RET ret = m_mpi->decode_put_packet(m_ctx, m_streamPacket);
        if (ret == MPP_OK) {
            packetDone = true;
            continue;
        }

        if (ret == MPP_ERR_BUFFER_FULL) {
            const int pollResult = pollFrames();
            if (pollResult < 0)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        setError(mppError("decode_put_packet", ret));
        return false;
    }

    for (int i = 0; i < (eos ? 2000 : 1); ++i) {
        const int ret = pollFrames();
        if (ret < 0)
            return false;
        if (ret > 0)
            return true;
        if (!eos)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (eos) {
        setError("等待 MPP EOS 输出超时");
        return false;
    }
    return true;
}

int MppDecoder::Impl::pollFrames()
{
    while (true) {
        MppFrame frame = nullptr;
        const MPP_RET ret = m_mpi->decode_get_frame(m_ctx, &frame);
        if (ret == MPP_ERR_TIMEOUT) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return 0;
        }
        if (ret != MPP_OK) {
            setError(mppError("decode_get_frame", ret));
            return -1;
        }
        if (frame == nullptr)
            return 0;

        const bool eos = mpp_frame_get_eos(frame) != 0;
        const bool ok = handleStreamFrame(frame);
        mpp_frame_deinit(&frame);
        if (!ok)
            return -1;
        if (eos)
            return 1;
    }
}

bool MppDecoder::Impl::handleStreamFrame(MppFrame frame)
{
    if (mpp_frame_get_info_change(frame) != 0)
        return handleInfoChange(frame);

    if (mpp_frame_get_errinfo(frame) != 0 || mpp_frame_get_discard(frame) != 0)
        return true;

    MppBuffer buffer = mpp_frame_get_buffer(frame);
    const int fd = buffer != nullptr ? mpp_buffer_get_fd(buffer) : -1;
    if (fd < 0) {
        setError("MPP 输出帧没有有效 dmaFd");
        return false;
    }

    VideoFrame output {};
    output.dmaFd = fd;
    output.va = buffer != nullptr ? mpp_buffer_get_ptr(buffer) : nullptr;
    output.width = static_cast<int>(mpp_frame_get_width(frame));
    output.height = static_cast<int>(mpp_frame_get_height(frame));
    output.stride = static_cast<int>(mpp_frame_get_hor_stride(frame));
    output.heightStride = static_cast<int>(mpp_frame_get_ver_stride(frame));
    output.capacity = mpp_frame_get_buf_size(frame);
    output.bytesUsed = output.capacity;
    output.format = fromMppFrameFormat(mpp_frame_get_fmt(frame));
    output.nativeFormat = static_cast<uint32_t>(mpp_frame_get_fmt(frame));
    output.timestampUs = static_cast<uint64_t>(mpp_frame_get_pts(frame));

    if (m_callback && !m_callback(output)) {
        setError("MppDecoder frame callback 返回失败");
        return false;
    }

    return true;
}

bool MppDecoder::Impl::handleInfoChange(MppFrame frame)
{
    const int width = static_cast<int>(mpp_frame_get_width(frame));
    const int height = static_cast<int>(mpp_frame_get_height(frame));
    const int stride = static_cast<int>(mpp_frame_get_hor_stride(frame));
    const int heightStride = static_cast<int>(mpp_frame_get_ver_stride(frame));
    const size_t mppSize = mpp_frame_get_buf_size(frame);
    const size_t requiredSize = std::max(mppOutputBufferSize(stride, heightStride), mppSize);

    if (width <= 0 || height <= 0 || requiredSize == 0) {
        setError("MPP info_change 返回无效输出尺寸");
        return false;
    }

    if (m_frameGroup == nullptr) {
        MPP_RET ret = mpp_buffer_group_get_external(&m_frameGroup, MPP_BUFFER_TYPE_EXT_DMA);
        if (ret != MPP_OK) {
            setError(mppError("mpp_buffer_group_get_external", ret));
            return false;
        }
    } else {
        MPP_RET ret = mpp_buffer_group_clear(m_frameGroup);
        if (ret != MPP_OK) {
            setError(mppError("mpp_buffer_group_clear", ret));
            return false;
        }
    }

    m_streamBuffers.clear();
    m_streamBuffers.reserve(kStreamOutputBufferCount);
    DmaAllocator allocator;
    for (int i = 0; i < kStreamOutputBufferCount; ++i) {
        DmaMemory memory;
        if (!allocator.allocate(requiredSize, memory)) {
            setError("MPP 输出 DMA buffer 分配失败: " + allocator.lastError());
            return false;
        }

        MppBufferInfo info {};
        info.type = MPP_BUFFER_TYPE_EXT_DMA;
        info.fd = memory.fd();
        info.size = requiredSize;
        info.index = i;

        const MPP_RET ret = mpp_buffer_commit(m_frameGroup, &info);
        if (ret != MPP_OK) {
            setError(mppError("mpp_buffer_commit", ret));
            return false;
        }

        m_streamBuffers.push_back(std::move(memory));
    }

    MPP_RET ret = m_mpi->control(m_ctx, MPP_DEC_SET_EXT_BUF_GROUP, m_frameGroup);
    if (ret != MPP_OK) {
        setError(mppError("MPP_DEC_SET_EXT_BUF_GROUP", ret));
        return false;
    }

    ret = m_mpi->control(m_ctx, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
    if (ret != MPP_OK) {
        setError(mppError("MPP_DEC_SET_INFO_CHANGE_READY", ret));
        return false;
    }

    return true;
}

void MppDecoder::Impl::setError(const std::string& message)
{
    m_lastError = message;
}
