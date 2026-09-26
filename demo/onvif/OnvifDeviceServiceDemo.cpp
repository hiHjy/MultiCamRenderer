#include "OnvifServer.hpp"

#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

namespace {

volatile std::sig_atomic_t g_stopRequested = 0;

extern "C" void onSignal(int)
{
    g_stopRequested = 1;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <http-port>\n";
        return 1;
    }

    // 本机 IPv4、MAC、稳定 Endpoint UUID，以及 main/sub RTSP URL 都由 OnvifServer
    // 使用当前网络身份自动派生；demo 不保留会因 DHCP 换址失效的本地 IP 参数。
    OnvifServerConfig config;
    try {
        const unsigned long port = std::stoul(argv[1]);
        if (port == 0 || port > 65535)
            throw std::out_of_range("port");
        config.deviceServicePort = static_cast<std::uint16_t>(port);
    } catch (...) {
        std::cerr << "invalid HTTP port\n";
        return 1;
    }

    OnvifServer server;
    if (!server.start(config)) {
        std::cerr << "ONVIF start failed: " << server.lastError() << '\n';
        return 1;
    }

    std::cout << "ONVIF ready: Device Service=" << server.deviceServiceUrl()
              << ", Discovery=239.255.255.250:3702; Ctrl+C to stop\n";
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    while (g_stopRequested == 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    server.stop();
    std::cout << "ONVIF stopped\n";
    return 0;
}
