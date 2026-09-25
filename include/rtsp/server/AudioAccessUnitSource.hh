#pragma once

#include <FramedSource.hh>

#include "AudioAccessUnitQueue.hh"

#include <memory>

/*
 * 把 AudioAccessUnitQueue 适配为 live555 FramedSource。
 * 每次 getNextFrame() 交付一个完整的裸 AAC-LC access unit；RTP AU header 由
 * MPEG4GenericRTPSink 生成，不在这里拼 ADTS 或 RTP 头。
 */
class AudioAccessUnitSource : public FramedSource {
public:
    static AudioAccessUnitSource* createNew(UsageEnvironment& env,
                                            std::shared_ptr<AudioAccessUnitQueue> queue);

private:
    AudioAccessUnitSource(UsageEnvironment& env, std::shared_ptr<AudioAccessUnitQueue> queue);
    ~AudioAccessUnitSource() override;

    void doGetNextFrame() override;
    void deliverAccessUnit();
    static void accessUnitArrivedCallback(void* clientData);

private:
    std::shared_ptr<AudioAccessUnitQueue> m_queue;
    EventTriggerId m_accessUnitArrivedTrigger = 0;
};
