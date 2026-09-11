#include "RtspStream.hpp"
#include "Sink.hpp"
#include "StreamManager.hpp"

#include <chrono>
#include <csignal>
#include <cstddef>
#include <iostream>
#include <memory>
#include <pthread.h>
#include <string>

namespace {

// 测试 Sink 只统计发布帧率，不保存 FramePacket；用于验证 Manager 的发布线程路径。
class StatisticsSink final : public Sink {
public:
    void onFrame(FramePacket packet) override
    {
        ++frameCount_;
        const auto now = std::chrono::steady_clock::now();
        const std::chrono::duration<double> elapsed = now - statisticsBegin_;
        if (elapsed.count() < 1.0) {
            return;
        }

        std::cout << "StreamManager publish fps=" << frameCount_ / elapsed.count()
                  << " streamId=" << packet.frame.streamId
                  << " layout=" << packet.frame.width << 'x' << packet.frame.height
                  << " sequence=" << packet.frame.sequence << '\n';
        frameCount_ = 0;
        statisticsBegin_ = now;
    }

private:
    std::size_t frameCount_ = 0;
    std::chrono::steady_clock::time_point statisticsBegin_ = std::chrono::steady_clock::now();
};

} // namespace

const char* streamStateName(StreamManager::StreamState state)
{
    switch (state) {
    case StreamManager::StreamState::Created:
        return "Created";
    case StreamManager::StreamState::Ready:
        return "Ready";
    case StreamManager::StreamState::Starting:
        return "Starting";
    case StreamManager::StreamState::Streaming:
        return "Streaming";
    case StreamManager::StreamState::Stopping:
        return "Stopping";
    case StreamManager::StreamState::Stopped:
        return "Stopped";
    case StreamManager::StreamState::Deleting:
        return "Deleting";
    case StreamManager::StreamState::Error:
        return "Error";
    }
    return "Unknown";
}

int main(int argc, char* argv[])
{
    sigset_t stopSignals;
    sigemptyset(&stopSignals);
    sigaddset(&stopSignals, SIGINT);
    sigaddset(&stopSignals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &stopSignals, nullptr) != 0) {
        std::cerr << "屏蔽退出信号失败\n";
        return 1;
    }

    StreamManager manager;
    const std::string url = argc > 1 ? argv[1] : "rtsp://192.168.1.5:8554/main";
    auto stream = std::make_shared<RtspStream>(url, 2);
    const int streamId = manager.addStream(stream);
    if (streamId < 0) {
        std::cerr << "添加 RtspStream 失败: " << manager.lastError() << '\n';
        return 1;
    }

    // FrameHub 仅以 weak_ptr 注册 Sink；业务方必须持有 Sink 的 shared_ptr 生命周期。
    const auto statisticsSink = std::make_shared<StatisticsSink>();
    if (!manager.addFrameSink(streamId, statisticsSink)) {
        std::cerr << "添加 StatisticsSink 失败: " << manager.lastError() << '\n';
        return 1;
    }
    if (!manager.startStream(streamId)) {
        std::cerr << "启动 RtspStream 失败: " << manager.lastError() << '\n';
        return 1;
    }

    std::cout << "正在通过 StreamManager 拉流并发布，按 Ctrl+C 停止\n";
    StreamManager::StreamState lastState = StreamManager::StreamState::Created;
    while (true) {
        const timespec timeout {0, 100 * 1000 * 1000};
        const int signal = sigtimedwait(&stopSignals, nullptr, &timeout);
        if (signal == SIGINT || signal == SIGTERM) {
            break;
        }

        StreamManager::StreamState state;
        std::string error;
        if (manager.getStreamState(streamId, state, &error) && state != lastState) {
            std::cout << "StreamManager state=" << streamStateName(state);
            if (!error.empty()) {
                std::cout << " error=" << error;
            }
            std::cout << '\n';
            lastState = state;
        }
    }

    (void)manager.stopStream(streamId);
    manager.shutdownPublishing();
    return 0;
}
