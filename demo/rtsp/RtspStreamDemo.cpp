#include "RtspStream.hpp"

#include <chrono>
#include <csignal>
#include <iostream>
#include <pthread.h>

int main()
{
    sigset_t stopSignals;
    sigemptyset(&stopSignals);
    sigaddset(&stopSignals, SIGINT);
    sigaddset(&stopSignals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &stopSignals, nullptr) != 0) {
        std::cerr << "屏蔽退出信号失败\n";
        return 1;
    }

    RtspStream stream("rtsp://192.168.1.5:8554/live", 2);
    if (!stream.start()) {
        std::cerr << "启动 RtspStream 失败: " << stream.lastError() << '\n';
        return 1;
    }

    size_t frameCount = 0;
    auto statisticsBegin = std::chrono::steady_clock::now();
    while (true) {
        FramePacket packet;
        while (stream.tryGetFrame(packet)) {
            ++frameCount;
            // packet 在循环下一次迭代或离开作用域后释放 lease，DMA buffer 回到 Stream pool。
        }

        const auto now = std::chrono::steady_clock::now();
        const std::chrono::duration<double> elapsed = now - statisticsBegin;
        if (elapsed.count() >= 1.0) {
            std::cout << "RtspStream frame/s=" << frameCount / elapsed.count() << '\n';
            frameCount = 0;
            statisticsBegin = now;
        }

        const timespec timeout {0, 10 * 1000 * 1000};
        const int signal = sigtimedwait(&stopSignals, nullptr, &timeout);
        if (signal == SIGINT || signal == SIGTERM) {
            break;
        }
    }

    return stream.stop() ? 0 : 1;
}
