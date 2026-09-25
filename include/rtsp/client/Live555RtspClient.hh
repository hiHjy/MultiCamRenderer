#ifndef LIVE555_RTSP_CLIENT_HH
#define LIVE555_RTSP_CLIENT_HH

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "AudioTypes.h"
#include "VideoCodec.hpp"

// 单路 RTSP 拉流封装。它只完成 RTSP/RTP 和 NALU 重组，不做 access unit 组帧，
// 也不做解码。MPP 已打开 split_parse 时，可直接连续接收每个 Annex-B NALU。
class Live555RtspClient {
public:
    // Connecting 表示 RTSP 控制面仍在 DESCRIBE/SETUP/PLAY 握手；Playing 只在收到
    // PLAY 200 OK 后上报。Error 是异步握手或 RTP 源异常结束，Stopped 是调用 stop()
    // 后事件线程完成退出。
    enum class State {
        Connecting,
        Playing,
        Stopped,
        Error,
    };

    // data 是完整的 Annex-B NALU（含 00 00 00 01）；只在本次回调期间有效。
    // 回调运行在本对象的 live555 事件线程，不能在其中执行长时间阻塞操作。
    using AnnexBNaluCallback = std::function<void(VideoCodec codec,
                                                   uint8_t* data,
                                                   size_t size,
                                                   uint64_t timestampUs)>;

    // AAC RTP 解包后的完整 access unit。packet.data 仅在回调期间有效；sourceFormat 和
    // frameSamples 来自 SDP，供 AudioPlaybackPipeline/后续 RTP 转发建立准确时钟。
    using AudioAccessUnitCallback = std::function<void(const AudioEncodedPacket& packet)>;
    using StateCallback = std::function<void(State state, const std::string& message)>;

    Live555RtspClient();
    ~Live555RtspClient();

    Live555RtspClient(const Live555RtspClient&) = delete;
    Live555RtspClient& operator=(const Live555RtspClient&) = delete;

    // 返回 true 仅表示事件线程已启动；状态由 stateCallback 异步上报。用户名密码可直接
    // 放到标准 rtsp://user:password@host/... URL。
    bool start(const std::string& url,
               AnnexBNaluCallback naluCallback,
               bool requestRtpOverTcp = false,
               StateCallback stateCallback = {},
               AudioAccessUnitCallback audioCallback = {});
    void stop();

    bool isRunning() const;
    VideoCodec codec() const;
    std::string lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // LIVE555_RTSP_CLIENT_HH
