#pragma once

#include <FramedSource.hh>

#include "AnnexBFrameQueue.hh"

#include <memory>

// 单个 live555 FramedSource 适配器：在 RTP sink 请求数据时从共享 Annex-B 队列取一个 NALU。
class AnnexBSource : public FramedSource {
public:
    static AnnexBSource* createNew(UsageEnvironment& env,
                                   std::shared_ptr<AnnexBFrameQueue> frameQueue);

private:
    AnnexBSource(UsageEnvironment& env, std::shared_ptr<AnnexBFrameQueue> frameQueue);
    ~AnnexBSource() override;

    void doGetNextFrame() override;
    void deliverFrame();
    static void frameArrivedCallback(void* clientData);

private:
    std::shared_ptr<AnnexBFrameQueue> m_frameQueue;
    EventTriggerId m_frameArrivedTrigger = 0;
};
