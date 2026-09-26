#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define ONVIF_CLIENT_TEXT_CAPACITY 512
#define ONVIF_CLIENT_MAX_PROFILES 16

/* C bridge 的输出全部是值对象，C++ 层无需持有 gSOAP 分配的临时响应内存。 */
typedef struct OnvifMediaServiceUrls {
    char mediaServiceUrl[ONVIF_CLIENT_TEXT_CAPACITY];
    char media2ServiceUrl[ONVIF_CLIENT_TEXT_CAPACITY];
    char error[ONVIF_CLIENT_TEXT_CAPACITY];
} OnvifMediaServiceUrls;

typedef struct OnvifDeviceStreamProfile {
    char token[ONVIF_CLIENT_TEXT_CAPACITY];
    char name[ONVIF_CLIENT_TEXT_CAPACITY];
    char streamUri[ONVIF_CLIENT_TEXT_CAPACITY];
    int fromMedia2;
} OnvifDeviceStreamProfile;

typedef struct OnvifDeviceStreamProfiles {
    unsigned int count;
    OnvifDeviceStreamProfile profiles[ONVIF_CLIENT_MAX_PROFILES];
    char error[ONVIF_CLIENT_TEXT_CAPACITY];
} OnvifDeviceStreamProfiles;

int onvif_device_get_media_service_urls(const char* deviceServiceUrl,
                                        OnvifMediaServiceUrls* result);
int onvif_media2_get_stream_profiles(const char* mediaServiceUrl,
                                     OnvifDeviceStreamProfiles* result);
int onvif_media1_get_stream_profiles(const char* mediaServiceUrl,
                                     OnvifDeviceStreamProfiles* result);

#ifdef __cplusplus
}
#endif
