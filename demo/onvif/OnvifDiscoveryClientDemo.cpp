#include "OnvifDeviceClient.h"
#include "OnvifDiscoveryClient.h"

#include <cstdlib>
#include <iostream>

namespace {

void printRequestFailure(const char* operation, OnvifClientResult result, const char* detail)
{
	if (result == ONVIF_CLIENT_AUTHENTICATION_REQUIRED) {
		std::cerr << "    " << operation
				  << " 需要 HTTP Digest 账号密码；上层应在此弹出登录框，然后使用同一 Device XAddr 重试。"
				  << std::endl;
	} else if (result == ONVIF_CLIENT_AUTHENTICATION_FAILED) {
		std::cerr << "    " << operation << " 的用户名或密码错误。" << std::endl;
	} else {
		std::cerr << "    " << operation << " 失败: " << detail << std::endl;
	}
}

} // namespace

int main(int argc, char *argv[]) {
	int timeoutMs = 3000;
	OnvifHttpDigestCredentials credentials{};
	const OnvifHttpDigestCredentials* credentialPtr = nullptr;
	if (argc == 2 || argc == 4) {
		timeoutMs = std::atoi(argv[1]);
		if (timeoutMs <= 0) {
			std::cerr << "用法: " << argv[0] << " [等待毫秒数，默认 3000] [用户名 密码]" << std::endl;
			return 2;
		}
		if (argc == 4) {
			credentials.username = argv[2];
			credentials.password = argv[3];
			credentialPtr = &credentials;
		}
	} else if (argc != 1) {
		std::cerr << "用法: " << argv[0] << " [等待毫秒数，默认 3000] [用户名 密码]" << std::endl;
		return 2;
	}

	// typedef struct OnvifDiscoveredDevice {
	// 	char endpointReference[ONVIF_CLIENT_TEXT_CAPACITY];
	// 	char deviceServiceUrl[ONVIF_CLIENT_TEXT_CAPACITY];
	// 	char types[ONVIF_CLIENT_TEXT_CAPACITY];
	// 	char scopes[ONVIF_CLIENT_TEXT_CAPACITY];
	// } OnvifDiscoveredDevice;

	// typedef struct OnvifDiscoveredDevices {
	// 	unsigned int count;
	// 	OnvifDiscoveredDevice devices[ONVIF_DISCOVERY_MAX_DEVICES];
	// 	char error[ONVIF_CLIENT_TEXT_CAPACITY];
	// } OnvifDiscoveredDevices;

	OnvifDiscoveredDevices devices{};
	unsigned int usableProfileCount = 0;
	if (onvif_discover_devices(static_cast<unsigned int>(timeoutMs), &devices) != 0) {
		std::cerr << "WS-Discovery 错误: " << devices.error << std::endl;
		return 1;
	}

	std::cout << "发现 " << devices.count << " 台 ONVIF 设备" << std::endl;
	for (unsigned int index = 0; index < devices.count; ++index) {
		const OnvifDiscoveredDevice &device = devices.devices[index];
		std::cout << "[" << index << "] EPR: " << device.endpointReference << '\n'
				  << "    Device XAddr: " << device.deviceServiceUrl << '\n'
				  << "    Types: " << device.types << '\n'
				  << "    Scopes: " << device.scopes << std::endl;

		// ONVIF 协议层统一是 C：控制调用结果已复制到固定 C 结构体，离开 gSOAP
		// context 后仍然有效。这里仅把结果打印出来，后续上层可交给 RtspClient。
		OnvifMediaServiceUrls serviceUrls{};
		//std::cout << device.deviceServiceUrl << std::endl;
		const OnvifClientResult servicesResult =
			onvif_device_get_media_service_urls(device.deviceServiceUrl, credentialPtr, &serviceUrls);
		if (servicesResult != ONVIF_CLIENT_OK) {
			printRequestFailure("Device.GetServices", servicesResult, serviceUrls.error);
			continue;
		}

		OnvifDeviceStreamProfiles profiles{};
		OnvifClientResult mediaResult = ONVIF_CLIENT_REQUEST_FAILED;
		if (serviceUrls.media2ServiceUrl[0] != '\0')
			mediaResult = onvif_media2_get_stream_profiles(serviceUrls.media2ServiceUrl, credentialPtr, &profiles);
		// Media2 明确表示“需要密码/密码错误”时，不能回退到 Media1 并覆盖这个状态；
		// 上层必须先拿到认证结果，才能决定弹登录框还是让用户重新输入。
		if (mediaResult == ONVIF_CLIENT_REQUEST_FAILED &&
			serviceUrls.mediaServiceUrl[0] != '\0') {
			mediaResult = onvif_media1_get_stream_profiles(serviceUrls.mediaServiceUrl, credentialPtr, &profiles);
		}
		if (mediaResult != ONVIF_CLIENT_OK || profiles.count == 0) {
			printRequestFailure("查询 Media Profile", mediaResult, profiles.error);
			continue;
		}
		for (unsigned int profileIndex = 0; profileIndex < profiles.count; ++profileIndex) {
			const OnvifDeviceStreamProfile &profile = profiles.profiles[profileIndex];
			std::cout << "    [" << (profile.fromMedia2 ? "Media2" : "Media") << "] " << profile.name
					  << " token=" << profile.token << '\n'
					  << "        RTSP: " << profile.streamUri << std::endl;
		}
		usableProfileCount += profiles.count;
	}
	// 仅发现 WS-Discovery 广播不等于能实际使用设备。认证失败、Media 不支持等情况
	// 都应当作为 demo 失败返回给脚本/CI。
	return usableProfileCount == 0 ? 1 : 0;
}
