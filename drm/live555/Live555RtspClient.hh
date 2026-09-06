#ifndef LIVE555_RTSP_CLIENT_HH
#define LIVE555_RTSP_CLIENT_HH

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "VideoCodec.hh"

// 单路 RTSP 拉流封装。它只完成 RTSP/RTP 和 NALU 重组，不做 access unit 组帧，
// 也不做解码。MPP 已打开 split_parse 时，可直接连续接收每个 Annex-B NALU。
class Live555RtspClient {
public:
    // data 是完整的 Annex-B NALU（含 00 00 00 01）；只在本次回调期间有效。
    // 回调运行在本对象的 live555 事件线程，不能在其中执行长时间阻塞操作。
    using AnnexBNaluCallback = std::function<void(VideoCodec codec,
                                                   uint8_t* data,
                                                   size_t size,
                                                   uint64_t timestampUs)>;

    Live555RtspClient();
    ~Live555RtspClient();

    Live555RtspClient(const Live555RtspClient&) = delete;
    Live555RtspClient& operator=(const Live555RtspClient&) = delete;

    // 返回 true 仅表示事件线程已启动；DESCRIBE/SETUP/PLAY 的异步错误通过
    // lastError() 查询。用户名密码可直接放到标准 rtsp://user:password@host/... URL。
    bool start(const std::string& url,
               AnnexBNaluCallback naluCallback,
               bool requestRtpOverTcp = false);
    void stop();

    bool isRunning() const;
    VideoCodec codec() const;
    std::string lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // LIVE555_RTSP_CLIENT_HH
