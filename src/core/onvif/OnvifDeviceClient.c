#include "OnvifDeviceClient.h"

#include "onvifStub.h"

#include <stdio.h>
#include <string.h>

#define ONVIF_MEDIA_NAMESPACE "http://www.onvif.org/ver10/media/wsdl"
#define ONVIF_MEDIA2_NAMESPACE "http://www.onvif.org/ver20/media/wsdl"

/* GetProfiles 后只需保留 token/name；避免在栈上为每条临时记录预留整份输出结构。 */
typedef struct OnvifProfileDescription {
    char token[ONVIF_CLIENT_TEXT_CAPACITY];
    char name[ONVIF_CLIENT_TEXT_CAPACITY];
} OnvifProfileDescription;

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

static void set_soap_error(char* destination, size_t capacity, const char* operation,
                           struct soap* soap)
{
    const char* fault = soap != NULL ? soap_fault_string(soap) : NULL;
    (void)snprintf(destination, capacity, "%s failed (gSOAP=%d)%s%s", operation,
                   soap != NULL ? soap->error : SOAP_EOF,
                   fault != NULL && *fault != '\0' ? ": " : "", fault != NULL ? fault : "");
}

static void init_soap(struct soap* soap)
{
    soap_init1(soap, SOAP_XML_TREE);
    soap->connect_timeout = 3;
    soap->send_timeout = 5;
    soap->recv_timeout = 5;
}

static void cleanup_soap(struct soap* soap)
{
    soap_destroy(soap);
    soap_end(soap);
    soap_done(soap);
}

int onvif_device_get_media_service_urls(const char* deviceServiceUrl,
                                        OnvifMediaServiceUrls* result)
{
    struct soap soap;
    struct _tds__GetServices request;
    struct _tds__GetServicesResponse response;
    int index;
    if (result == NULL)
        return -1;
    memset(result, 0, sizeof(*result));
    if (deviceServiceUrl == NULL || *deviceServiceUrl == '\0') {
        copy_text(result->error, sizeof(result->error), "Device Service XAddr is empty");
        return -1;
    }

    init_soap(&soap);
    memset(&request, 0, sizeof(request));
    memset(&response, 0, sizeof(response));
    request.IncludeCapability = xsd__boolean__false_;
    if (soap_call___tds__GetServices(&soap, deviceServiceUrl, NULL, &request, &response) != SOAP_OK) {
        set_soap_error(result->error, sizeof(result->error), "Device.GetServices", &soap);
        cleanup_soap(&soap);
        return -1;
    }
    if (response.__sizeService > 0 && response.Service == NULL) {
        copy_text(result->error, sizeof(result->error), "Device.GetServices returned an invalid service list");
        cleanup_soap(&soap);
        return -1;
    }
    for (index = 0; index < response.__sizeService; ++index) {
        const struct tds__Service* service = &response.Service[index];
        if (service->Namespace == NULL)
            continue;
        if (strcmp(service->Namespace, ONVIF_MEDIA2_NAMESPACE) == 0)
            copy_text(result->media2ServiceUrl, sizeof(result->media2ServiceUrl), service->XAddr);
        else if (strcmp(service->Namespace, ONVIF_MEDIA_NAMESPACE) == 0)
            copy_text(result->mediaServiceUrl, sizeof(result->mediaServiceUrl), service->XAddr);
    }
    cleanup_soap(&soap);
    if (result->mediaServiceUrl[0] == '\0' && result->media2ServiceUrl[0] == '\0') {
        copy_text(result->error, sizeof(result->error), "Device.GetServices returned no Media Service");
        return -1;
    }
    return 0;
}

static int append_media2_profile(const char* endpoint, const char* token, const char* name,
                                 OnvifDeviceStreamProfiles* result)
{
    struct soap soap;
    struct _tr2__GetStreamUri request;
    struct _tr2__GetStreamUriResponse response;
    OnvifDeviceStreamProfile* output;
    if (result->count >= ONVIF_CLIENT_MAX_PROFILES)
        return 0;
    init_soap(&soap);
    memset(&request, 0, sizeof(request));
    memset(&response, 0, sizeof(response));
    request.Protocol = "RtspUnicast";
    request.ProfileToken = (char*)token;
    if (soap_call___tr2__GetStreamUri(&soap, endpoint, NULL, &request, &response) != SOAP_OK) {
        set_soap_error(result->error, sizeof(result->error), "Media2.GetStreamUri", &soap);
        cleanup_soap(&soap);
        return -1;
    }
    if (response.Uri == NULL || response.Uri[0] == '\0') {
        copy_text(result->error, sizeof(result->error), "Media2.GetStreamUri returned an empty RTSP URI");
        cleanup_soap(&soap);
        return -1;
    }
    output = &result->profiles[result->count++];
    copy_text(output->token, sizeof(output->token), token);
    copy_text(output->name, sizeof(output->name), name);
    copy_text(output->streamUri, sizeof(output->streamUri), response.Uri);
    output->fromMedia2 = 1;
    cleanup_soap(&soap);
    return 0;
}

int onvif_media2_get_stream_profiles(const char* mediaServiceUrl,
                                     OnvifDeviceStreamProfiles* result)
{
    struct soap soap;
    struct _tr2__GetProfiles request;
    struct _tr2__GetProfilesResponse response;
    OnvifProfileDescription descriptions[ONVIF_CLIENT_MAX_PROFILES];
    unsigned int descriptionCount = 0;
    int index;
    if (result == NULL)
        return -1;
    memset(result, 0, sizeof(*result));
    if (mediaServiceUrl == NULL || *mediaServiceUrl == '\0') {
        copy_text(result->error, sizeof(result->error), "Media2 service URL is empty");
        return -1;
    }
    init_soap(&soap);
    memset(&request, 0, sizeof(request));
    memset(&response, 0, sizeof(response));
    if (soap_call___tr2__GetProfiles(&soap, mediaServiceUrl, NULL, &request, &response) != SOAP_OK) {
        set_soap_error(result->error, sizeof(result->error), "Media2.GetProfiles", &soap);
        cleanup_soap(&soap);
        return -1;
    }
    if (response.__sizeProfiles > 0 && response.Profiles == NULL) {
        copy_text(result->error, sizeof(result->error), "Media2.GetProfiles returned an invalid profile list");
        cleanup_soap(&soap);
        return -1;
    }
    memset(descriptions, 0, sizeof(descriptions));
    for (index = 0; index < response.__sizeProfiles && descriptionCount < ONVIF_CLIENT_MAX_PROFILES; ++index) {
        const struct tr2__MediaProfile* profile = &response.Profiles[index];
        if (profile->token == NULL || *profile->token == '\0')
            continue;
        copy_text(descriptions[descriptionCount].token, sizeof(descriptions[descriptionCount].token), profile->token);
        copy_text(descriptions[descriptionCount].name, sizeof(descriptions[descriptionCount].name), profile->Name);
        ++descriptionCount;
    }
    cleanup_soap(&soap);
    for (index = 0; index < (int)descriptionCount; ++index)
        (void)append_media2_profile(mediaServiceUrl, descriptions[index].token, descriptions[index].name, result);
    if (result->count == 0 && result->error[0] == '\0')
        copy_text(result->error, sizeof(result->error), "Media2.GetProfiles returned no usable stream profiles");
    return result->count > 0 ? 0 : -1;
}

static int append_media1_profile(const char* endpoint, const char* token, const char* name,
                                 OnvifDeviceStreamProfiles* result)
{
    struct soap soap;
    struct tt__Transport transport;
    struct tt__StreamSetup setup;
    struct _trt__GetStreamUri request;
    struct _trt__GetStreamUriResponse response;
    OnvifDeviceStreamProfile* output;
    if (result->count >= ONVIF_CLIENT_MAX_PROFILES)
        return 0;
    init_soap(&soap);
    memset(&transport, 0, sizeof(transport));
    memset(&setup, 0, sizeof(setup));
    memset(&request, 0, sizeof(request));
    memset(&response, 0, sizeof(response));
    transport.Protocol = tt__TransportProtocol__RTSP;
    setup.Stream = tt__StreamType__RTP_Unicast;
    setup.Transport = &transport;
    request.StreamSetup = &setup;
    request.ProfileToken = (char*)token;
    if (soap_call___trt__GetStreamUri(&soap, endpoint, NULL, &request, &response) != SOAP_OK) {
        set_soap_error(result->error, sizeof(result->error), "Media.GetStreamUri", &soap);
        cleanup_soap(&soap);
        return -1;
    }
    if (response.MediaUri == NULL || response.MediaUri->Uri == NULL || response.MediaUri->Uri[0] == '\0') {
        copy_text(result->error, sizeof(result->error), "Media.GetStreamUri returned an empty RTSP URI");
        cleanup_soap(&soap);
        return -1;
    }
    output = &result->profiles[result->count++];
    copy_text(output->token, sizeof(output->token), token);
    copy_text(output->name, sizeof(output->name), name);
    copy_text(output->streamUri, sizeof(output->streamUri), response.MediaUri != NULL ? response.MediaUri->Uri : NULL);
    output->fromMedia2 = 0;
    cleanup_soap(&soap);
    return 0;
}

int onvif_media1_get_stream_profiles(const char* mediaServiceUrl,
                                     OnvifDeviceStreamProfiles* result)
{
    struct soap soap;
    struct _trt__GetProfiles request;
    struct _trt__GetProfilesResponse response;
    OnvifProfileDescription descriptions[ONVIF_CLIENT_MAX_PROFILES];
    unsigned int descriptionCount = 0;
    int index;
    if (result == NULL)
        return -1;
    memset(result, 0, sizeof(*result));
    if (mediaServiceUrl == NULL || *mediaServiceUrl == '\0') {
        copy_text(result->error, sizeof(result->error), "Media service URL is empty");
        return -1;
    }
    init_soap(&soap);
    memset(&request, 0, sizeof(request));
    memset(&response, 0, sizeof(response));
    if (soap_call___trt__GetProfiles(&soap, mediaServiceUrl, NULL, &request, &response) != SOAP_OK) {
        set_soap_error(result->error, sizeof(result->error), "Media.GetProfiles", &soap);
        cleanup_soap(&soap);
        return -1;
    }
    if (response.__sizeProfiles > 0 && response.Profiles == NULL) {
        copy_text(result->error, sizeof(result->error), "Media.GetProfiles returned an invalid profile list");
        cleanup_soap(&soap);
        return -1;
    }
    memset(descriptions, 0, sizeof(descriptions));
    for (index = 0; index < response.__sizeProfiles && descriptionCount < ONVIF_CLIENT_MAX_PROFILES; ++index) {
        const struct tt__Profile* profile = &response.Profiles[index];
        if (profile->token == NULL || *profile->token == '\0')
            continue;
        copy_text(descriptions[descriptionCount].token, sizeof(descriptions[descriptionCount].token), profile->token);
        copy_text(descriptions[descriptionCount].name, sizeof(descriptions[descriptionCount].name), profile->Name);
        ++descriptionCount;
    }
    cleanup_soap(&soap);
    for (index = 0; index < (int)descriptionCount; ++index)
        (void)append_media1_profile(mediaServiceUrl, descriptions[index].token, descriptions[index].name, result);
    if (result->count == 0 && result->error[0] == '\0')
        copy_text(result->error, sizeof(result->error), "Media.GetProfiles returned no usable stream profiles");
    return result->count > 0 ? 0 : -1;
}
