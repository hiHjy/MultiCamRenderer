#ifndef ANNEX_B_SINK_HH
#define ANNEX_B_SINK_HH

#include <MediaSink.hh>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "VideoCodec.hh"

// live555 的 RTP 解包器把一个完整 NALU 交给 MediaSink。这个类补上 Annex-B
// 起始码后同步调用上层回调；data 的有效期只到回调返回为止。
class AnnexBSink : public MediaSink {
public:
    using NaluCallback = std::function<void(VideoCodec codec,
                                            uint8_t* data,
                                            size_t size,
                                            uint64_t timestampUs)>;
    using ErrorCallback = std::function<void(const std::string& message)>;

    static AnnexBSink* createNew(UsageEnvironment& env,
                                 VideoCodec codec,
                                 NaluCallback naluCallback,
                                 ErrorCallback errorCallback,
                                 unsigned maxNaluBytes);

private:
    AnnexBSink(UsageEnvironment& env,
               VideoCodec codec,
               NaluCallback naluCallback,
               ErrorCallback errorCallback,
               unsigned maxNaluBytes);
    ~AnnexBSink() override;

    static void afterGettingFrame(void* clientData,
                                  unsigned frameSize,
                                  unsigned numTruncatedBytes,
                                  timeval presentationTime,
                                  unsigned durationInMicroseconds);
    void afterGettingFrame(unsigned frameSize,
                           unsigned numTruncatedBytes,
                           timeval presentationTime,
                           unsigned durationInMicroseconds);
    Boolean continuePlaying() override;

private:
    static constexpr size_t kAnnexBStartCodeSize = 4;

    std::vector<uint8_t> receiveBuffer_;
    VideoCodec codec_;
    NaluCallback naluCallback_;
    ErrorCallback errorCallback_;
};

#endif // ANNEX_B_SINK_HH
