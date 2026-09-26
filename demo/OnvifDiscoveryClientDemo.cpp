#include "OnvifDiscoveryClient.h"
#include "OnvifDeviceClient.h"

#include <cstdlib>
#include <iostream>

int main(int argc, char* argv[])
{
    int timeoutMs = 3000;
    if (argc == 2) {
        timeoutMs = std::atoi(argv[1]);
        if (timeoutMs <= 0) {
            std::cerr << "用法: " << argv[0] << " [等待毫秒数，默认 3000]" << std::endl;
            return 2;
        }
    } else if (argc != 1) {
        std::cerr << "用法: " << argv[0] << " [等待毫秒数，默认 3000]" << std::endl;
        return 2;
    }

    OnvifDiscoveredDevices devices {};
    if (onvif_discover_devices(static_cast<unsigned int>(timeoutMs), &devices) != 0) {
        std::cerr << "WS-Discovery 错误: " << devices.error << std::endl;
        return 1;
    }

    std::cout << "发现 " << devices.count << " 台 ONVIF 设备" << std::endl;
    for (unsigned int index = 0; index < devices.count; ++index) {
        const OnvifDiscoveredDevice& device = devices.devices[index];
        std::cout << "[" << index << "] EPR: " << device.endpointReference << '\n'
                  << "    Device XAddr: " << device.deviceServiceUrl << '\n'
                  << "    Types: " << device.types << '\n'
                  << "    Scopes: " << device.scopes << std::endl;

        // ONVIF 协议层统一是 C：控制调用结果已复制到固定 C 结构体，离开 gSOAP
        // context 后仍然有效。这里仅把结果打印出来，后续上层可交给 RtspClient。
        OnvifMediaServiceUrls serviceUrls {};
        if (onvif_device_get_media_service_urls(device.deviceServiceUrl, &serviceUrls) != 0) {
            std::cerr << "    Device.GetServices 失败: " << serviceUrls.error << std::endl;
            continue;
        }

        OnvifDeviceStreamProfiles profiles {};
        int media2Result = -1;
        if (serviceUrls.media2ServiceUrl[0] != '\0')
            media2Result = onvif_media2_get_stream_profiles(serviceUrls.media2ServiceUrl, &profiles);
        if (media2Result != 0 && serviceUrls.mediaServiceUrl[0] != '\0')
            (void)onvif_media1_get_stream_profiles(serviceUrls.mediaServiceUrl, &profiles);
        if (profiles.count == 0) {
            std::cerr << "    查询 Media Profile 失败: " << profiles.error << std::endl;
            continue;
        }
        for (unsigned int profileIndex = 0; profileIndex < profiles.count; ++profileIndex) {
            const OnvifDeviceStreamProfile& profile = profiles.profiles[profileIndex];
            std::cout << "    [" << (profile.fromMedia2 ? "Media2" : "Media") << "] "
                      << profile.name << " token=" << profile.token << '\n'
                      << "        RTSP: " << profile.streamUri << std::endl;
        }
    }
    return devices.count == 0 ? 1 : 0;
}
