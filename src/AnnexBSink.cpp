#include "AnnexBSink.hh"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

AnnexBSink* AnnexBSink::createNew(UsageEnvironment& env,
                                  VideoCodec codec,
                                  NaluCallback naluCallback,
                                  ErrorCallback errorCallback,
                                  unsigned maxNaluBytes)
{
    if (!naluCallback || maxNaluBytes == 0) {
        return nullptr;
    }
    return new AnnexBSink(env, codec, std::move(naluCallback), std::move(errorCallback), maxNaluBytes);
}

AnnexBSink::AnnexBSink(UsageEnvironment& env,
                       VideoCodec codec,
                       NaluCallback naluCallback,
                       ErrorCallback errorCallback,
                       unsigned maxNaluBytes)
    : MediaSink(env),
      receiveBuffer_(static_cast<size_t>(maxNaluBytes) + kAnnexBStartCodeSize),
      codec_(codec),
      naluCallback_(std::move(naluCallback)),
      errorCallback_(std::move(errorCallback))
{
    static constexpr uint8_t kAnnexBStartCode[kAnnexBStartCodeSize] = {0, 0, 0, 1};
    std::memcpy(receiveBuffer_.data(), kAnnexBStartCode, sizeof(kAnnexBStartCode));
}

AnnexBSink::~AnnexBSink() = default;

void AnnexBSink::afterGettingFrame(void* clientData,
                                   unsigned frameSize,
                                   unsigned numTruncatedBytes,
                                   timeval presentationTime,
                                   unsigned durationInMicroseconds)
{
    static_cast<AnnexBSink*>(clientData)->afterGettingFrame(
        frameSize, numTruncatedBytes, presentationTime, durationInMicroseconds);
}

// 关键数据回调：向上层交付带 Annex-B 起始码的完整 NALU。
void AnnexBSink::afterGettingFrame(unsigned frameSize,
                                   unsigned numTruncatedBytes,
                                   timeval presentationTime,
                                   unsigned /*durationInMicroseconds*/)
{
    if (numTruncatedBytes != 0) {
        const std::string message = "接收缓冲区不足，丢弃截断 NALU：保留 "
            + std::to_string(frameSize) + " 字节，丢失 " + std::to_string(numTruncatedBytes) + " 字节";
        envir() << message.c_str() << "\n";
        if (errorCallback_) {
            errorCallback_(message);
        }
    } else {
        const uint64_t timestampUs = presentationTime.tv_sec < 0
            ? 0
            : static_cast<uint64_t>(presentationTime.tv_sec) * 1000000ULL
                + static_cast<uint64_t>(presentationTime.tv_usec);
        // receiveBuffer_ 的前四字节在构造时已写入 Annex-B 起始码。
        // 项目禁止用 C++ 异常传递错误；回调内部自行记录错误并正常返回。
        // 这是 Live555RtspClient::start() 传入的上层回调；buffer 仅在本次回调期间有效。
        naluCallback_(codec_, receiveBuffer_.data(), kAnnexBStartCodeSize + frameSize, timestampUs);
    }

    // MediaSink 不是主动轮询；处理完本次 NALU 后必须继续请求下一次。
    continuePlaying(); // 向 live555 登记下一条 NALU 的接收请求。
}

Boolean AnnexBSink::continuePlaying()
{
    if (fSource == nullptr) {
        return False;
    }

    fSource->getNextFrame(receiveBuffer_.data() + kAnnexBStartCodeSize,
                          static_cast<unsigned>(receiveBuffer_.size() - kAnnexBStartCodeSize),
                          afterGettingFrame,
                          this,
                          onSourceClosure,
                          this);
    return True;
}
