#include "OnvifSoapService.h"

#include "stdsoap2.h"
#include "wsddapi.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define ONVIF_DEVICE_NAMESPACE "http://www.onvif.org/ver10/device/wsdl"
#define ONVIF_MEDIA_NAMESPACE "http://www.onvif.org/ver10/media/wsdl"
#define ONVIF_WSDD_PORT 3702
#define ONVIF_WSDD_MULTICAST_ADDRESS "239.255.255.250"
#define ONVIF_NETWORK_VIDEO_TRANSMITTER "dn:NetworkVideoTransmitter"
// ONVIF Discovery Feature 规范要求 ProbeMatch 同时声明通用 Device 与 NVT 类型。
// 只声明 NVT 虽然很多客户端能识别，但 ODM 会用两种类型交叉过滤。
#define ONVIF_DISCOVERY_TYPES "dn:NetworkVideoTransmitter tds:Device"

/* gSOAP owns all response memory through the current struct soap. */
static void *soap_calloc(struct soap *soap, size_t size)
{
    void *memory = soap_malloc(soap, size);
    if (memory != NULL)
        memset(memory, 0, size);
    return memory;
}

static char *soap_copy_text(struct soap *soap, const char *text)
{
    const size_t length = strlen(text) + 1;
    char *copy = (char *)soap_malloc(soap, length);
    if (copy != NULL)
        memcpy(copy, text, length);
    return copy;
}

static char *soap_copy_text_range(struct soap *soap, const char *begin, size_t length)
{
    char *copy = (char *)soap_malloc(soap, length + 1);
    if (copy != NULL) {
        memcpy(copy, begin, length);
        copy[length] = '\0';
    }
    return copy;
}

static enum xsd__boolean *soap_boolean(struct soap *soap, enum xsd__boolean value)
{
    enum xsd__boolean *result = (enum xsd__boolean *)soap_calloc(soap, sizeof(*result));
    if (result != NULL)
        *result = value;
    return result;
}

static const OnvifSoapServiceConfig *service_config(const struct soap *soap)
{
    return (const OnvifSoapServiceConfig *)soap->user;
}

static int stop_requested(OnvifSoapStopRequestedFn callback, void *context)
{
    return callback != NULL && callback(context) != 0;
}

static void report_started(OnvifSoapStartedFn callback, void *context,
                           int success, const char *error_text)
{
    if (callback != NULL)
        callback(context, success, error_text);
}

static int populate_utc_time(struct soap *soap, struct tt__SystemDateTime **output)
{
    time_t now;
    struct tm utc;
    struct tt__SystemDateTime *system_time;
    struct tt__DateTime *date_time;
    struct tt__Date *date;
    struct tt__Time *utc_time;

    now = time(NULL);
    if (gmtime_r(&now, &utc) == NULL)
        return SOAP_EOM;

    system_time = (struct tt__SystemDateTime *)soap_calloc(soap, sizeof(*system_time));
    date_time = (struct tt__DateTime *)soap_calloc(soap, sizeof(*date_time));
    date = (struct tt__Date *)soap_calloc(soap, sizeof(*date));
    utc_time = (struct tt__Time *)soap_calloc(soap, sizeof(*utc_time));
    if (system_time == NULL || date_time == NULL || date == NULL || utc_time == NULL)
        return SOAP_EOM;

    system_time->DateTimeType = tt__SetDateTimeType__Manual;
    system_time->DaylightSavings = xsd__boolean__false_;
    system_time->UTCDateTime = date_time;
    date_time->Date = date;
    date_time->Time = utc_time;
    date->Year = utc.tm_year + 1900;
    date->Month = utc.tm_mon + 1;
    date->Day = utc.tm_mday;
    utc_time->Hour = utc.tm_hour;
    utc_time->Minute = utc.tm_min;
    utc_time->Second = utc.tm_sec;
    *output = system_time;
    return SOAP_OK;
}

/* Required by gSOAP's generic dispatcher. ONVIF Device Service does not
 * consume incoming Fault messages, so accepting one is sufficient here. */
int SOAP_ENV__Fault(struct soap *soap, char *faultcode, char *faultstring,
                    char *faultactor, struct SOAP_ENV__Detail *detail,
                    struct SOAP_ENV__Code *code, struct SOAP_ENV__Reason *reason,
                    char *node, char *role, struct SOAP_ENV__Detail *detail12)
{
    (void)soap; (void)faultcode; (void)faultstring; (void)faultactor; (void)detail;
    (void)code; (void)reason; (void)node; (void)role; (void)detail12;
    return SOAP_OK;
}

/*
 * The following six globals are the intentionally implemented part of the
 * complete Device WSDL. gSOAP parses the request before it calls us and
 * serializes `response` after SOAP_OK is returned. All other generated Device
 * operations return SOAP_NO_METHOD from onvifUnimplementedOps.c.
 */
int __tds__GetDeviceInformation(struct soap *soap,
                                struct _tds__GetDeviceInformation *request,
                                struct _tds__GetDeviceInformationResponse *response)
{
    const OnvifSoapServiceConfig *config = service_config(soap);
    (void)request;
    if (config == NULL)
        return SOAP_FAULT;
    response->Manufacturer = soap_copy_text(soap, config->manufacturer);
    response->Model = soap_copy_text(soap, config->model);
    response->FirmwareVersion = soap_copy_text(soap, config->firmwareVersion);
    response->SerialNumber = soap_copy_text(soap, config->serialNumber);
    response->HardwareId = soap_copy_text(soap, config->hardwareId);
    return response->Manufacturer != NULL && response->Model != NULL &&
                   response->FirmwareVersion != NULL && response->SerialNumber != NULL &&
                   response->HardwareId != NULL
               ? SOAP_OK
               : SOAP_EOM;
}

int __tds__GetSystemDateAndTime(struct soap *soap,
                                struct _tds__GetSystemDateAndTime *request,
                                struct _tds__GetSystemDateAndTimeResponse *response)
{
    (void)request;
    return populate_utc_time(soap, &response->SystemDateAndTime);
}

int __tds__GetScopes(struct soap *soap, struct _tds__GetScopes *request,
                     struct _tds__GetScopesResponse *response)
{
    const OnvifSoapServiceConfig *config = service_config(soap);
    const char *cursor;
    int count = 0;
    int index = 0;
    (void)request;
    if (config == NULL)
        return SOAP_FAULT;

    for (cursor = config->scopes; *cursor != '\0';) {
        while (*cursor == ' ')
            ++cursor;
        if (*cursor == '\0')
            break;
        ++count;
        while (*cursor != '\0' && *cursor != ' ')
            ++cursor;
    }
    response->__sizeScopes = count;
    response->Scopes = (struct tt__Scope *)soap_calloc(soap, (size_t)count * sizeof(*response->Scopes));
    if (response->Scopes == NULL)
        return SOAP_EOM;

    for (cursor = config->scopes; *cursor != '\0';) {
        const char *begin;
        while (*cursor == ' ')
            ++cursor;
        if (*cursor == '\0')
            break;
        begin = cursor;
        while (*cursor != '\0' && *cursor != ' ')
            ++cursor;
        response->Scopes[index].ScopeDef = tt__ScopeDefinition__Fixed;
        response->Scopes[index].ScopeItem = soap_copy_text_range(soap, begin, (size_t)(cursor - begin));
        if (response->Scopes[index].ScopeItem == NULL)
            return SOAP_EOM;
        ++index;
    }
    return SOAP_OK;
}

int __tds__GetServiceCapabilities(struct soap *soap,
                                  struct _tds__GetServiceCapabilities *request,
                                  struct _tds__GetServiceCapabilitiesResponse *response)
{
    struct tds__DeviceServiceCapabilities *capabilities;
    (void)request;
    capabilities = (struct tds__DeviceServiceCapabilities *)soap_calloc(soap, sizeof(*capabilities));
    if (capabilities == NULL)
        return SOAP_EOM;
    capabilities->Network = (struct tds__NetworkCapabilities *)soap_calloc(soap, sizeof(*capabilities->Network));
    capabilities->Security = (struct tds__SecurityCapabilities *)soap_calloc(soap, sizeof(*capabilities->Security));
    capabilities->System = (struct tds__SystemCapabilities *)soap_calloc(soap, sizeof(*capabilities->System));
    if (capabilities->Network == NULL || capabilities->Security == NULL || capabilities->System == NULL)
        return SOAP_EOM;
    /* Authentication is introduced only when the full ONVIF auth policy exists. */
    capabilities->System->DiscoveryResolve = soap_boolean(soap, xsd__boolean__false_);
    if (capabilities->System->DiscoveryResolve == NULL)
        return SOAP_EOM;
    response->Capabilities = capabilities;
    return SOAP_OK;
}

int __tds__GetServices(struct soap *soap, struct _tds__GetServices *request,
                       struct _tds__GetServicesResponse *response)
{
    const OnvifSoapServiceConfig *config = service_config(soap);
    struct tds__Service *services;
    struct tt__OnvifVersion *versions;
    (void)request;
    if (config == NULL)
        return SOAP_FAULT;
    services = (struct tds__Service *)soap_calloc(soap, 2 * sizeof(*services));
    versions = (struct tt__OnvifVersion *)soap_calloc(soap, 2 * sizeof(*versions));
    if (services == NULL || versions == NULL)
        return SOAP_EOM;
    response->__sizeService = 2;
    response->Service = services;
    services[0].Namespace = soap_copy_text(soap, ONVIF_DEVICE_NAMESPACE);
    services[0].XAddr = soap_copy_text(soap, config->deviceServiceUrl);
    services[0].Version = &versions[0];
    versions[0].Major = 25;
    versions[0].Minor = 6;
    services[1].Namespace = soap_copy_text(soap, ONVIF_MEDIA_NAMESPACE);
    services[1].XAddr = soap_copy_text(soap, config->mediaServiceUrl);
    services[1].Version = &versions[1];
    versions[1].Major = 1;
    versions[1].Minor = 0;
    return services[0].Namespace != NULL && services[0].XAddr != NULL &&
                   services[1].Namespace != NULL && services[1].XAddr != NULL
               ? SOAP_OK
               : SOAP_EOM;
}

int __tds__GetCapabilities(struct soap *soap, struct _tds__GetCapabilities *request,
                           struct _tds__GetCapabilitiesResponse *response)
{
    const OnvifSoapServiceConfig *config = service_config(soap);
    struct tt__Capabilities *capabilities;
    struct tt__DeviceCapabilities *device;
    struct tt__MediaCapabilities *media;
    struct tt__RealTimeStreamingCapabilities *streaming;
    (void)request;
    if (config == NULL)
        return SOAP_FAULT;
    capabilities = (struct tt__Capabilities *)soap_calloc(soap, sizeof(*capabilities));
    device = (struct tt__DeviceCapabilities *)soap_calloc(soap, sizeof(*device));
    media = (struct tt__MediaCapabilities *)soap_calloc(soap, sizeof(*media));
    streaming = (struct tt__RealTimeStreamingCapabilities *)soap_calloc(soap, sizeof(*streaming));
    if (capabilities == NULL || device == NULL || media == NULL || streaming == NULL)
        return SOAP_EOM;
    device->XAddr = soap_copy_text(soap, config->deviceServiceUrl);
    media->XAddr = soap_copy_text(soap, config->mediaServiceUrl);
    streaming->RTP_USCORERTSP_USCORETCP = soap_boolean(soap, xsd__boolean__true_);
    if (device->XAddr == NULL || media->XAddr == NULL || streaming->RTP_USCORERTSP_USCORETCP == NULL)
        return SOAP_EOM;
    media->StreamingCapabilities = streaming;
    capabilities->Device = device;
    capabilities->Media = media;
    response->Capabilities = capabilities;
    return SOAP_OK;
}

static int fill_aac_configuration(struct soap *soap, struct tt__Profile *profile, const char *token)
{
    struct tt__AudioEncoderConfiguration *audio =
        (struct tt__AudioEncoderConfiguration *)soap_calloc(soap, sizeof(*audio));
    if (audio == NULL)
        return SOAP_EOM;
    audio->Name = soap_copy_text(soap, "AAC-LC 48kHz");
    audio->token = soap_copy_text(soap, token);
    audio->Encoding = tt__AudioEncoding__AAC;
    audio->Bitrate = 128;
    audio->SampleRate = 48;
    audio->UseCount = 1;
    audio->SessionTimeout = soap_copy_text(soap, "PT60S");
    if (audio->Name == NULL || audio->token == NULL || audio->SessionTimeout == NULL)
        return SOAP_EOM;
    profile->AudioEncoderConfiguration = audio;
    return SOAP_OK;
}

static int fill_h264_sub_configuration(struct soap *soap, struct tt__Profile *profile)
{
    struct tt__VideoEncoderConfiguration *video =
        (struct tt__VideoEncoderConfiguration *)soap_calloc(soap, sizeof(*video));
    struct tt__VideoResolution *resolution =
        (struct tt__VideoResolution *)soap_calloc(soap, sizeof(*resolution));
    struct tt__VideoRateControl *rate_control =
        (struct tt__VideoRateControl *)soap_calloc(soap, sizeof(*rate_control));
    struct tt__H264Configuration *h264 =
        (struct tt__H264Configuration *)soap_calloc(soap, sizeof(*h264));
    if (video == NULL || resolution == NULL || rate_control == NULL || h264 == NULL)
        return SOAP_EOM;
    video->Name = soap_copy_text(soap, "sub H264 1280x720");
    video->token = soap_copy_text(soap, "sub_video_h264");
    video->Encoding = tt__VideoEncoding__H264;
    video->Resolution = resolution;
    video->Quality = 5.0F;
    video->RateControl = rate_control;
    video->H264 = h264;
    video->UseCount = 1;
    video->SessionTimeout = soap_copy_text(soap, "PT60S");
    resolution->Width = 1280;
    resolution->Height = 720;
    rate_control->FrameRateLimit = 30;
    rate_control->EncodingInterval = 1;
    rate_control->BitrateLimit = 2048;
    h264->GovLength = 30;
    h264->H264Profile = tt__H264Profile__High;
    if (video->Name == NULL || video->token == NULL || video->SessionTimeout == NULL)
        return SOAP_EOM;
    profile->VideoEncoderConfiguration = video;
    return SOAP_OK;
}

static int fill_media_profile(struct soap *soap, struct tt__Profile *profile,
                              const char *token, const char *name, int is_h264)
{
    profile->Name = soap_copy_text(soap, name);
    profile->token = soap_copy_text(soap, token);
    profile->fixed = soap_boolean(soap, xsd__boolean__true_);
    if (profile->Name == NULL || profile->token == NULL || profile->fixed == NULL)
        return SOAP_EOM;
    if (fill_aac_configuration(soap, profile, is_h264 ? "sub_audio_aac" : "main_audio_aac") != SOAP_OK)
        return SOAP_EOM;
    /* Media v1 has no H265 enum. Do not lie about main's codec: clients obtain
     * its exact H265 SDP from the returned RTSP URI. Media2 adds H265 metadata later. */
    return is_h264 ? fill_h264_sub_configuration(soap, profile) : SOAP_OK;
}

int __trt__GetServiceCapabilities(struct soap *soap,
                                  struct _trt__GetServiceCapabilities *request,
                                  struct _trt__GetServiceCapabilitiesResponse *response)
{
    struct trt__Capabilities *capabilities;
    struct trt__ProfileCapabilities *profiles;
    struct trt__StreamingCapabilities *streaming;
    (void)request;
    capabilities = (struct trt__Capabilities *)soap_calloc(soap, sizeof(*capabilities));
    profiles = (struct trt__ProfileCapabilities *)soap_calloc(soap, sizeof(*profiles));
    streaming = (struct trt__StreamingCapabilities *)soap_calloc(soap, sizeof(*streaming));
    if (capabilities == NULL || profiles == NULL || streaming == NULL)
        return SOAP_EOM;
    profiles->MaximumNumberOfProfiles = (int *)soap_calloc(soap, sizeof(*profiles->MaximumNumberOfProfiles));
    streaming->RTP_USCORERTSP_USCORETCP = soap_boolean(soap, xsd__boolean__true_);
    if (profiles->MaximumNumberOfProfiles == NULL || streaming->RTP_USCORERTSP_USCORETCP == NULL)
        return SOAP_EOM;
    *profiles->MaximumNumberOfProfiles = 2;
    capabilities->ProfileCapabilities = profiles;
    capabilities->StreamingCapabilities = streaming;
    response->Capabilities = capabilities;
    return SOAP_OK;
}

int __trt__GetProfiles(struct soap *soap, struct _trt__GetProfiles *request,
                       struct _trt__GetProfilesResponse *response)
{
    struct tt__Profile *profiles;
    (void)request;
    profiles = (struct tt__Profile *)soap_calloc(soap, 2 * sizeof(*profiles));
    if (profiles == NULL)
        return SOAP_EOM;
    if (fill_media_profile(soap, &profiles[0], "main", "Main Stream (H265)", 0) != SOAP_OK ||
        fill_media_profile(soap, &profiles[1], "sub", "Sub Stream (H264)", 1) != SOAP_OK) {
        return SOAP_EOM;
    }
    response->__sizeProfiles = 2;
    response->Profiles = profiles;
    return SOAP_OK;
}

int __trt__GetProfile(struct soap *soap, struct _trt__GetProfile *request,
                      struct _trt__GetProfileResponse *response)
{
    struct tt__Profile *profile;
    const int is_sub = request != NULL && request->ProfileToken != NULL &&
                       strcmp(request->ProfileToken, "sub") == 0;
    if (request == NULL || request->ProfileToken == NULL ||
        (strcmp(request->ProfileToken, "main") != 0 && !is_sub)) {
        return SOAP_FAULT;
    }
    profile = (struct tt__Profile *)soap_calloc(soap, sizeof(*profile));
    if (profile == NULL)
        return SOAP_EOM;
    if (fill_media_profile(soap, profile, is_sub ? "sub" : "main",
                           is_sub ? "Sub Stream (H264)" : "Main Stream (H265)", is_sub) != SOAP_OK) {
        return SOAP_EOM;
    }
    response->Profile = profile;
    return SOAP_OK;
}

int __trt__GetStreamUri(struct soap *soap, struct _trt__GetStreamUri *request,
                        struct _trt__GetStreamUriResponse *response)
{
    const OnvifSoapServiceConfig *config = service_config(soap);
    const char *rtsp_url;
    struct tt__MediaUri *media_uri;
    if (config == NULL || request == NULL || request->ProfileToken == NULL)
        return SOAP_FAULT;
    if (strcmp(request->ProfileToken, "main") == 0)
        rtsp_url = config->mainRtspUrl;
    else if (strcmp(request->ProfileToken, "sub") == 0)
        rtsp_url = config->subRtspUrl;
    else
        return SOAP_FAULT;
    media_uri = (struct tt__MediaUri *)soap_calloc(soap, sizeof(*media_uri));
    if (media_uri == NULL)
        return SOAP_EOM;
    media_uri->Uri = soap_copy_text(soap, rtsp_url);
    media_uri->InvalidAfterConnect = xsd__boolean__false_;
    media_uri->InvalidAfterReboot = xsd__boolean__false_;
    media_uri->Timeout = soap_copy_text(soap, "PT0S");
    if (media_uri->Uri == NULL || media_uri->Timeout == NULL)
        return SOAP_EOM;
    response->MediaUri = media_uri;
    return SOAP_OK;
}

static int probe_requests_this_device_type(const char *types)
{
    /* ODM 会同时探测 ONVIF 具体设备类型 dn:NetworkVideoTransmitter 与通用的
       tds:Device。后者不是“不匹配”，而是它发现任意 ONVIF Device Service 的方式。 */
    return types == NULL || *types == '\0' ||
           strstr(types, "NetworkVideoTransmitter") != NULL || strstr(types, ":Device") != NULL;
}

static int get_peer_udp_endpoint(const struct soap *soap, char *endpoint, size_t endpoint_size)
{
    char host[NI_MAXHOST] = {0};
    char service[NI_MAXSERV] = {0};
    const int result = getnameinfo((const struct sockaddr *)&soap->peer, soap->peerlen,
                                   host, sizeof(host), service, sizeof(service),
                                   NI_NUMERICHOST | NI_NUMERICSERV);
    const int written = snprintf(endpoint, endpoint_size, "soap.udp://%s:%s", host, service);
    return result == 0 && written > 0 && (size_t)written < endpoint_size ? 0 : -1;
}

/* gSOAP's wsddapi plugin requires all six hooks. We only act on Probe: this
 * IPC is a discovery target, not a discovery client and has no Resolve yet. */
void wsdd_event_Hello(struct soap *soap, unsigned int a, const char *b, unsigned int c,
                      const char *d, const char *e, const char *f, const char *g,
                      const char *h, const char *i, const char *j, unsigned int k)
{ (void)soap; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; (void)j; (void)k; }
void wsdd_event_Bye(struct soap *soap, unsigned int a, const char *b, unsigned int c,
                    const char *d, const char *e, const char *f, const char *g,
                    const char *h, const char *i, const char *j, unsigned int *k)
{ (void)soap; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; (void)j; (void)k; }

soap_wsdd_mode wsdd_event_Probe(struct soap *soap, const char *message_id,
                                const char *reply_to, const char *types,
                                const char *scopes, const char *match_by,
                                struct wsdd__ProbeMatchesType *matches)
{
    const OnvifSoapServiceConfig *config = service_config(soap);
    char reply_endpoint[NI_MAXHOST + NI_MAXSERV + 16] = {0};
    (void)reply_to; (void)scopes; (void)match_by;
    if (config == NULL || message_id == NULL || !probe_requests_this_device_type(types))
        return SOAP_WSDD_ADHOC;
    soap_wsdd_init_ProbeMatches(soap, matches);
    if (soap_wsdd_add_ProbeMatch(soap, matches, config->endpointReference,
                                 ONVIF_DISCOVERY_TYPES, config->scopes, NULL,
                                 config->deviceServiceUrl, 1) != SOAP_OK ||
        get_peer_udp_endpoint(soap, reply_endpoint, sizeof(reply_endpoint)) != 0 ||
        soap_wsdd_ProbeMatches(soap, reply_endpoint, soap_wsa_rand_uuid(soap), message_id,
                               soap_wsa_anonymousURI, matches) != SOAP_OK) {
        fprintf(stderr, "OnvifSoapService: reply ProbeMatches failed soap=%d errno=%d\n",
                soap->error, soap->errnum);
        return SOAP_WSDD_ADHOC;
    }
    return SOAP_WSDD_ADHOC;
}
void wsdd_event_ProbeMatches(struct soap *soap, unsigned int a, const char *b, unsigned int c,
                             const char *d, const char *e, struct wsdd__ProbeMatchesType *f)
{ (void)soap; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; }
soap_wsdd_mode wsdd_event_Resolve(struct soap *soap, const char *a, const char *b,
                                  const char *c, struct wsdd__ResolveMatchType *d)
{ (void)soap; (void)a; (void)b; (void)c; (void)d; return SOAP_WSDD_ADHOC; }
void wsdd_event_ResolveMatches(struct soap *soap, unsigned int a, const char *b, unsigned int c,
                               const char *d, const char *e, struct wsdd__ResolveMatchType *f)
{ (void)soap; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; }

int onvif_soap_run_device_service(const OnvifSoapServiceConfig *config, unsigned short port,
                                  OnvifSoapStopRequestedFn should_stop, void *stop_context,
                                  OnvifSoapStartedFn on_started, void *started_context)
{
    struct soap soap;
    char error[128];
    if (config == NULL || port == 0) {
        report_started(on_started, started_context, 0, "invalid Device Service configuration");
        return -1;
    }
    soap_init1(&soap, SOAP_XML_TREE);
    soap.user = (void *)config;
    /* Rebinding after an app restart must not wait for old TCP TIME_WAIT sockets. */
    soap.bind_flags = SO_REUSEADDR;
    if (!soap_valid_socket(soap_bind(&soap, NULL, port, 16))) {
        snprintf(error, sizeof(error), "bind HTTP port %u failed (soap=%d errno=%d)",
                 (unsigned int)port, soap.error, soap.errnum);
        report_started(on_started, started_context, 0, error);
        soap_done(&soap);
        return -1;
    }
    report_started(on_started, started_context, 1, "");
    /* Timed accept makes stop() bounded even when there are no HTTP clients. */
    soap.accept_timeout = 1;
    while (!stop_requested(should_stop, stop_context)) {
        if (!soap_valid_socket(soap_accept(&soap))) {
            if (soap.errnum != 0 && !stop_requested(should_stop, stop_context))
                fprintf(stderr, "OnvifSoapService: HTTP accept error soap=%d errno=%d\n", soap.error, soap.errnum);
            continue;
        }
        if (soap_serve(&soap) != SOAP_OK)
            soap_print_fault(&soap, stderr);
        soap_destroy(&soap);
        soap_end(&soap);
        soap.user = (void *)config;
    }
    soap_done(&soap);
    return 0;
}

int onvif_soap_run_discovery_service(const OnvifSoapServiceConfig *config,
                                     OnvifSoapStopRequestedFn should_stop, void *stop_context,
                                     OnvifSoapStartedFn on_started, void *started_context)
{
    struct soap soap;
    struct ip_mreq membership;
    char error[128];
    if (config == NULL) {
        report_started(on_started, started_context, 0, "invalid WS-Discovery configuration");
        return -1;
    }
    soap_init1(&soap, SOAP_IO_UDP | SOAP_XML_TREE);
    soap.user = (void *)config;
    if (soap_register_plugin(&soap, soap_wsa) != SOAP_OK ||
        !soap_valid_socket(soap_bind(&soap, NULL, ONVIF_WSDD_PORT, 16))) {
        snprintf(error, sizeof(error), "bind WS-Discovery UDP %d failed (soap=%d errno=%d)",
                 ONVIF_WSDD_PORT, soap.error, soap.errnum);
        report_started(on_started, started_context, 0, error);
        soap_done(&soap);
        return -1;
    }
    memset(&membership, 0, sizeof(membership));
    membership.imr_multiaddr.s_addr = inet_addr(ONVIF_WSDD_MULTICAST_ADDRESS);
    membership.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(soap.master, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) != 0) {
        snprintf(error, sizeof(error), "join WS-Discovery multicast failed (errno=%d)", errno);
        report_started(on_started, started_context, 0, error);
        soap_done(&soap);
        return -1;
    }
    report_started(on_started, started_context, 1, "");
    /* ONVIF Target Service 上线时应主动 Hello。部分管理工具的刷新界面只监听
       Hello，或因 Windows 防火墙过滤了它自己发出的 Probe；主动公告能兼容这类实现。 */
    if (soap_wsdd_Hello(&soap, SOAP_WSDD_ADHOC,
                        "soap.udp://" ONVIF_WSDD_MULTICAST_ADDRESS ":3702",
                        soap_wsa_rand_uuid(&soap), NULL, config->endpointReference,
                        ONVIF_DISCOVERY_TYPES, config->scopes, NULL,
                        config->deviceServiceUrl, 1) != SOAP_OK) {
        fprintf(stderr, "OnvifSoapService: send Hello failed soap=%d errno=%d\n", soap.error, soap.errnum);
    } else {
        fprintf(stderr, "OnvifSoapService: sent Hello endpoint=%s xaddr=%s\n",
                config->endpointReference, config->deviceServiceUrl);
    }
    while (!stop_requested(should_stop, stop_context)) {
        const int result = soap_wsdd_listen(&soap, -200000);
        if (result != SOAP_OK && !stop_requested(should_stop, stop_context) && soap.errnum != 0)
            fprintf(stderr, "OnvifSoapService: WS-Discovery receive error soap=%d errno=%d\n", soap.error, soap.errnum);
    }
    soap_destroy(&soap);
    soap_end(&soap);
    soap_done(&soap);
    return 0;
}
