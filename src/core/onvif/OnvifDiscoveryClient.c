#include "OnvifDiscoveryClient.h"
#include "OnvifWsddClient.h"

#include <stdio.h>
#include <string.h>

static void copy_text(char* destination, size_t capacity, const char* source)
{
    size_t length;
    if (destination == NULL || capacity == 0)
        return;
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    length = strlen(source);
    if (length >= capacity)
        length = capacity - 1;
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static int is_duplicate(const OnvifDiscoveredDevices* result, const OnvifDiscoveredDevice* device)
{
    unsigned int index;
    for (index = 0; index < result->count; ++index) {
        const OnvifDiscoveredDevice* existing = &result->devices[index];
        if (device->endpointReference[0] != '\0' && existing->endpointReference[0] != '\0' &&
            strcmp(device->endpointReference, existing->endpointReference) == 0) {
            return 1;
        }
        if (device->deviceServiceUrl[0] != '\0' && existing->deviceServiceUrl[0] != '\0' &&
            strcmp(device->deviceServiceUrl, existing->deviceServiceUrl) == 0) {
            return 1;
        }
    }
    return 0;
}

int onvif_discover_devices(unsigned int timeoutMs, OnvifDiscoveredDevices* result)
{
    OnvifWsddProbeResult probeResult;
    unsigned int index;
    if (result == NULL)
        return -1;
    memset(result, 0, sizeof(*result));
    if (timeoutMs == 0)
        timeoutMs = 3000;
    if (timeoutMs > 30000)
        timeoutMs = 30000;
    memset(&probeResult, 0, sizeof(probeResult));
    if (onvif_wsdd_probe(timeoutMs, &probeResult) != 0) {
        copy_text(result->error, sizeof(result->error), probeResult.error);
        return -1;
    }

    for (index = 0; index < probeResult.count && result->count < ONVIF_DISCOVERY_MAX_DEVICES; ++index) {
        const OnvifWsddProbeMatch* match = &probeResult.matches[index];
        OnvifDiscoveredDevice device;
        memset(&device, 0, sizeof(device));
        copy_text(device.endpointReference, sizeof(device.endpointReference), match->endpointReference);
        copy_text(device.deviceServiceUrl, sizeof(device.deviceServiceUrl), match->deviceServiceUrl);
        copy_text(device.types, sizeof(device.types), match->types);
        copy_text(device.scopes, sizeof(device.scopes), match->scopes);
        if (!is_duplicate(result, &device))
            result->devices[result->count++] = device;
    }
    return 0;
}
