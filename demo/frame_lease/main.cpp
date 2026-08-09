#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

struct VideoFrame {
    int bufferIndex = -1;
    int sequence = 0;
    int width = 640;
    int height = 480;
};

class FrameLease {
public:
    FrameLease(VideoFrame frame, std::function<void(int)> release)
        : m_frame(frame), m_release(std::move(release))
    {
        std::cout << "[lease] create frame seq=" << m_frame.sequence
                  << " buf=" << m_frame.bufferIndex << "\n";
    }

    ~FrameLease()
    {
        std::cout << "[lease] destroy frame seq=" << m_frame.sequence
                  << " buf=" << m_frame.bufferIndex << ", return to camera\n";
        if (m_release)
            m_release(m_frame.bufferIndex);
    }

    const VideoFrame& frame() const { return m_frame; }

private:
    VideoFrame m_frame;
    std::function<void(int)> m_release;
};

using FrameLeasePtr = std::shared_ptr<FrameLease>;

class LatestFrameSink {
public:
    LatestFrameSink(std::string name, std::chrono::milliseconds processTime)
        : m_name(std::move(name)), m_processTime(processTime), m_thread(&LatestFrameSink::run, this)
    {
    }

    ~LatestFrameSink()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_cv.notify_all();
        if (m_thread.joinable())
            m_thread.join();
    }

    void onFrame(FrameLeasePtr lease)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_latest) {
                std::cout << "[" << m_name << "] drop old seq="
                          << (*m_latest)->frame().sequence << "\n";
            }
            m_latest = std::move(lease);
        }
        m_cv.notify_one();
    }

private:
    void run()
    {
        while (true) {
            FrameLeasePtr lease;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return m_stopping || m_latest.has_value(); });
                if (m_stopping && !m_latest)
                    break;

                lease = std::move(*m_latest);
                m_latest.reset();
            }

            const auto& frame = lease->frame();
            std::cout << "[" << m_name << "] start process seq=" << frame.sequence
                      << " buf=" << frame.bufferIndex << "\n";
            std::this_thread::sleep_for(m_processTime);
            std::cout << "[" << m_name << "] finish process seq=" << frame.sequence
                      << " buf=" << frame.bufferIndex << "\n";

            // lease 在这里离开作用域。若这是最后一个 shared_ptr，
            // FrameLease 析构函数会把 V4L2 buffer 归还给摄像头。
        }
    }

    std::string m_name;
    std::chrono::milliseconds m_processTime;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::optional<FrameLeasePtr> m_latest;
    bool m_stopping = false;
    std::thread m_thread;
};

class FakeCamera {
public:
    explicit FakeCamera(int bufferCount)
    {
        for (int i = 0; i < bufferCount; ++i)
            m_freeBuffers.push(i);
    }

    void addSink(std::weak_ptr<LatestFrameSink> sink)
    {
        m_sinks.push_back(std::move(sink));
    }

    void run(int frameCount, std::chrono::milliseconds interval)
    {
        for (int i = 0; i < frameCount; ++i) {
            int bufferIndex = waitDqbuf();
            VideoFrame frame {
                .bufferIndex = bufferIndex,
                .sequence = ++m_sequence,
            };

            auto lease = std::make_shared<FrameLease>(
                frame,
                [this](int index) {
                    // 真实工程里建议把 index 投递回采集线程，
                    // 由采集线程统一执行 QBUF。这里为了演示直接归还。
                    qbuf(index);
                });

            std::cout << "[camera] publish seq=" << frame.sequence
                      << " buf=" << frame.bufferIndex << "\n";
            publish(lease);

            // 摄像头发布后立刻丢掉自己的引用。
            // 此后只有 sink 持有这帧；最后一个 sink 释放后才会 QBUF。
            lease.reset();

            std::this_thread::sleep_for(interval);
        }
    }

private:
    int waitDqbuf()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return !m_freeBuffers.empty(); });

        int index = m_freeBuffers.front();
        m_freeBuffers.pop();
        std::cout << "[camera] DQBUF buf=" << index
                  << " free_left=" << m_freeBuffers.size() << "\n";
        return index;
    }

    void qbuf(int index)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_freeBuffers.push(index);
            std::cout << "[camera] QBUF  buf=" << index
                      << " free_now=" << m_freeBuffers.size() << "\n";
        }
        m_cv.notify_one();
    }

    void publish(const FrameLeasePtr& lease)
    {
        for (auto it = m_sinks.begin(); it != m_sinks.end();) {
            if (auto sink = it->lock()) {
                sink->onFrame(lease);
                ++it;
            } else {
                it = m_sinks.erase(it);
            }
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::queue<int> m_freeBuffers;
    std::vector<std::weak_ptr<LatestFrameSink>> m_sinks;
    int m_sequence = 0;
};

int main()
{
    auto displaySink = std::make_shared<LatestFrameSink>("display", 40ms);
    auto recordSink = std::make_shared<LatestFrameSink>("record", 120ms);

    FakeCamera camera(4);
    camera.addSink(displaySink);
    camera.addSink(recordSink);

    camera.run(12, 33ms);

    displaySink.reset();
    recordSink.reset();

    std::cout << "done\n";
    return 0;
}
