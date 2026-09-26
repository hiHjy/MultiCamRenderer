#include "Live555RtspClient.hh"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr size_t kMaxPendingNalus = 1024;

// live555 回调运行在 RTP 收包事件线程。这里仅复制 NALU 入队，由独立线程落盘，
// 防止文件 I/O 导致事件线程来不及处理后续 RTP 包。
class AnnexBRecorder {
public:
    explicit AnnexBRecorder(std::string outputPath)
        : m_outputPath(std::move(outputPath))
    {
    }

    ~AnnexBRecorder()
    {
        stop();
    }

    bool start()
    {
        m_output.open(m_outputPath, std::ios::binary | std::ios::trunc);
        if (!m_output.is_open()) {
            std::cerr << "无法打开输出文件: " << m_outputPath << '\n';
            return false;
        }

        m_writer = std::thread(&AnnexBRecorder::writerMain, this);
        return true;
    }

    void enqueue(const uint8_t* data, size_t size)
    {
        if (data == nullptr || size == 0) {
            return;
        }

        Nalu nalu;
        nalu.annexB.assign(data, data + size);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping) {
                return;
            }

            if (m_queue.size() >= kMaxPendingNalus) {
                ++m_queueDroppedNalus;
                return;
            }
            m_queue.emplace_back(std::move(nalu));
        }
        m_cv.notify_one();
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping) {
                return;
            }
            m_stopping = true;
        }
        m_cv.notify_one();

        if (m_writer.joinable()) {
            m_writer.join();
        }
        if (m_output.is_open()) {
            m_output.close();
        }
    }

    size_t writtenNalus() const
    {
        return m_writtenNalus.load();
    }

    size_t writtenBytes() const
    {
        return m_writtenBytes.load();
    }

    size_t queueDroppedNalus() const
    {
        return m_queueDroppedNalus.load();
    }

    bool hasWriteError() const
    {
        return m_writeError.load();
    }

private:
    struct Nalu {
        std::vector<uint8_t> annexB;
    };

    void writerMain()
    {
        for (;;) {
            Nalu nalu;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
                if (m_queue.empty()) {
                    if (m_stopping) {
                        break;
                    }
                    continue;
                }
                nalu = std::move(m_queue.front());
                m_queue.pop_front();
            }

            m_output.write(reinterpret_cast<const char*>(nalu.annexB.data()),
                           static_cast<std::streamsize>(nalu.annexB.size()));
            if (!m_output.good()) {
                m_writeError.store(true);
                std::cerr << "写入录制文件失败: " << m_outputPath << '\n';
                return;
            }
            ++m_writtenNalus;
            m_writtenBytes.fetch_add(nalu.annexB.size());
        }
    }

    std::string m_outputPath;
    std::ofstream m_output;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<Nalu> m_queue;
    std::thread m_writer;
    bool m_stopping = false;
    std::atomic<size_t> m_writtenNalus{0};
    std::atomic<size_t> m_writtenBytes{0};
    std::atomic<size_t> m_queueDroppedNalus{0};
    std::atomic<bool> m_writeError{false};
};

} // namespace

int main(int argc, char* argv[])
{
    const std::string url = argc > 1 ? argv[1] : "rtsp://192.168.1.5:8554/main";
    const std::string outputPath = argc > 2 ? argv[2] : "/tmp/rtsp-record.h265";
    unsigned int durationSeconds = 15;
    if (argc > 3) {
        char* parseEnd = nullptr;
        errno = 0;
        const unsigned long parsed = std::strtoul(argv[3], &parseEnd, 10);
        if (errno != 0 || parseEnd == argv[3] || *parseEnd != '\0' || parsed == 0 || parsed > UINT32_MAX) {
            std::cerr << "录制秒数必须是有效正整数: " << argv[3] << '\n';
            return 1;
        }
        durationSeconds = static_cast<unsigned int>(parsed);
    }
    const bool useTcp = argc > 4 && std::string(argv[4]) == "tcp";

    // 在创建 live555/写文件线程前屏蔽信号，由主线程同步接收，避免 signal handler
    // 直接触碰 C++ 对象。
    sigset_t stopSignals;
    sigemptyset(&stopSignals);
    sigaddset(&stopSignals, SIGINT);
    sigaddset(&stopSignals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &stopSignals, nullptr) != 0) {
        std::cerr << "屏蔽退出信号失败" << '\n';
        return 1;
    }

    AnnexBRecorder recorder(outputPath);
    if (!recorder.start()) {
        return 1;
    }

    std::atomic<bool> rtspError{false};
    Live555RtspClient client;
    if (!client.start(
            url,
            [&recorder](VideoCodec, uint8_t* data, size_t size, uint64_t) {
                recorder.enqueue(data, size);
            },
            useTcp,
            [&rtspError](Live555RtspClient::State state, const std::string& message) {
                if (state == Live555RtspClient::State::Playing) {
                    std::cout << "RTSP 已开始接收" << '\n';
                } else if (state == Live555RtspClient::State::Error) {
                    std::cerr << "RTSP 异步失败: " << message << '\n';
                    rtspError.store(true);
                }
            })) {
        std::cerr << "RTSP 启动失败: " << client.lastError() << '\n';
        recorder.stop();
        return 1;
    }

    std::cout << "录制 " << url << " -> " << outputPath
              << "，时长=" << durationSeconds << " 秒，传输=" << (useTcp ? "TCP" : "UDP") << '\n';
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(durationSeconds);
    bool stoppedBySignal = false;
    while (!rtspError.load() && std::chrono::steady_clock::now() < deadline) {
        const timespec timeout{0, 100 * 1000 * 1000};
        const int receivedSignal = sigtimedwait(&stopSignals, nullptr, &timeout);
        if (receivedSignal == SIGINT || receivedSignal == SIGTERM) {
            stoppedBySignal = true;
            break;
        }
        if (receivedSignal == -1 && errno != EAGAIN && errno != EINTR) {
            std::cerr << "等待退出信号失败" << '\n';
            rtspError.store(true);
            break;
        }
    }

    client.stop();
    recorder.stop();

    std::cout << "录制完成: NALU=" << recorder.writtenNalus()
              << " bytes=" << recorder.writtenBytes()
              << " writerDropped=" << recorder.queueDroppedNalus()
              << (stoppedBySignal ? "（已由信号提前停止）" : "") << '\n';
    if (recorder.hasWriteError() || recorder.queueDroppedNalus() != 0 || recorder.writtenNalus() == 0) {
        return 2;
    }
    return rtspError.load() ? 2 : 0;
}
