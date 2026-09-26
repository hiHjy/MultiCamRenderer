#pragma once

#include "OnvifDeviceClient.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ONVIF_DISCOVERY_MAX_DEVICES 32

/* 一条 WS-Discovery ProbeMatch；字符串均已复制，不依赖 gSOAP 的临时内存。 */
typedef struct OnvifDiscoveredDevice {
    char endpointReference[ONVIF_CLIENT_TEXT_CAPACITY];
    char deviceServiceUrl[ONVIF_CLIENT_TEXT_CAPACITY];
    char types[ONVIF_CLIENT_TEXT_CAPACITY];
    char scopes[ONVIF_CLIENT_TEXT_CAPACITY];
} OnvifDiscoveredDevice;

typedef struct OnvifDiscoveredDevices {
    unsigned int count;
    OnvifDiscoveredDevice devices[ONVIF_DISCOVERY_MAX_DEVICES];
    char error[ONVIF_CLIENT_TEXT_CAPACITY];
} OnvifDiscoveredDevices;

/* 同步发送 WS-Discovery Probe；timeoutMs 为等待单播 ProbeMatch 的总时长。 */
int onvif_discover_devices(unsigned int timeoutMs, OnvifDiscoveredDevices* result);

#ifdef __cplusplus
}
#endif
