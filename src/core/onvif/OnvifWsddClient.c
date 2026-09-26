#include "OnvifWsddClient.h"

#include "stdsoap2.h"
#include "wsddapi.h"

#include <stdio.h>
#include <string.h>

#define ONVIF_WSDD_ENDPOINT "soap.udp://239.255.255.250:3702"
#define ONVIF_WSDD_TYPES "dn:NetworkVideoTransmitter"
#define ONVIF_WSDD_DEFAULT_TIMEOUT_US 3000000U
#define ONVIF_WSDD_MAX_TIMEOUT_US 30000000U

static void copy_text(char* destination, size_t destinationSize, const char* source)
{
    if (destination == NULL || destinationSize == 0)
        return;
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    (void)snprintf(destination, destinationSize, "%s", source);
}

static void copy_first_xaddr(char* destination, size_t destinationSize, const char* xaddrs)
{
    const char* begin = xaddrs;
    const char* end;
    size_t length;
    if (destination == NULL || destinationSize == 0)
        return;
    while (begin != NULL && (*begin == ' ' || *begin == '\t' || *begin == '\r' || *begin == '\n'))
        ++begin;
    if (begin == NULL || *begin == '\0') {
        destination[0] = '\0';
        return;
    }
    end = begin;
    while (*end != '\0' && *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n')
        ++end;
    length = (size_t)(end - begin);
    if (length >= destinationSize)
        length = destinationSize - 1;
    memcpy(destination, begin, length);
    destination[length] = '\0';
}

static void set_error(OnvifWsddProbeResult* result, const char* operation, const struct soap* soap)
{
    if (result == NULL)
        return;
    (void)snprintf(result->error, sizeof(result->error), "%s: soap=%d errno=%d", operation,
                   soap != NULL ? soap->error : -1, soap != NULL ? soap->errnum : 0);
}

/* ProbeMatch 允许 UDP 重传；必须在固定槽位耗尽前去重，不能等到上层再处理。 */
static int is_duplicate_match(const OnvifWsddProbeResult* result,
                              const OnvifWsddProbeMatch* candidate)
{
    unsigned int index;
    for (index = 0; index < result->count; ++index) {
        const OnvifWsddProbeMatch* existing = &result->matches[index];
        if (candidate->endpointReference[0] != '\0' && existing->endpointReference[0] != '\0' &&
            strcmp(candidate->endpointReference, existing->endpointReference) == 0) {
            return 1;
        }
        if (candidate->deviceServiceUrl[0] != '\0' && existing->deviceServiceUrl[0] != '\0' &&
            strcmp(candidate->deviceServiceUrl, existing->deviceServiceUrl) == 0) {
            return 1;
        }
    }
    return 0;
}

/* wsddapi is a C plug-in and resolves these six global callbacks by name. */
void wsdd_event_Hello(struct soap* soap, unsigned int instanceId, const char* sequenceId,
                      unsigned int messageNumber, const char* messageId, const char* relatesTo,
                      const char* endpointReference, const char* types, const char* scopes,
                      const char* matchBy, const char* xaddrs, unsigned int metadataVersion)
{
    (void)soap; (void)instanceId; (void)sequenceId; (void)messageNumber; (void)messageId;
    (void)relatesTo; (void)endpointReference; (void)types; (void)scopes; (void)matchBy;
    (void)xaddrs; (void)metadataVersion;
}

void wsdd_event_Bye(struct soap* soap, unsigned int instanceId, const char* sequenceId,
                    unsigned int messageNumber, const char* messageId, const char* relatesTo,
                    const char* endpointReference, const char* types, const char* scopes,
                    const char* matchBy, const char* xaddrs, unsigned int* metadataVersion)
{
    (void)soap; (void)instanceId; (void)sequenceId; (void)messageNumber; (void)messageId;
    (void)relatesTo; (void)endpointReference; (void)types; (void)scopes; (void)matchBy;
    (void)xaddrs; (void)metadataVersion;
}

soap_wsdd_mode wsdd_event_Probe(struct soap* soap, const char* messageId, const char* replyTo,
                                const char* types, const char* scopes, const char* matchBy,
                                struct wsdd__ProbeMatchesType* matches)
{
    (void)soap; (void)messageId; (void)replyTo; (void)types; (void)scopes; (void)matchBy;
    (void)matches;
    return SOAP_WSDD_ADHOC;
}

void wsdd_event_ProbeMatches(struct soap* soap, unsigned int instanceId, const char* sequenceId,
                             unsigned int messageNumber, const char* messageId,
                             const char* relatesTo, struct wsdd__ProbeMatchesType* matches)
{
    OnvifWsddProbeResult* result = soap != NULL ? (OnvifWsddProbeResult*)soap->user : NULL;
    int index;
    (void)instanceId; (void)sequenceId; (void)messageNumber; (void)messageId; (void)relatesTo;
    if (result == NULL || matches == NULL || matches->ProbeMatch == NULL)
        return;
    for (index = 0; index < matches->__sizeProbeMatch && result->count < ONVIF_WSDD_MAX_MATCHES; ++index) {
        const struct wsdd__ProbeMatchType* match = &matches->ProbeMatch[index];
        OnvifWsddProbeMatch candidate;
        memset(&candidate, 0, sizeof(candidate));
        copy_text(candidate.endpointReference, sizeof(candidate.endpointReference),
                  match->wsa5__EndpointReference.Address);
        copy_first_xaddr(candidate.deviceServiceUrl, sizeof(candidate.deviceServiceUrl), match->XAddrs);
        copy_text(candidate.types, sizeof(candidate.types), match->Types);
        copy_text(candidate.scopes, sizeof(candidate.scopes),
                  match->Scopes != NULL ? match->Scopes->__item : NULL);
        if (candidate.deviceServiceUrl[0] != '\0' && !is_duplicate_match(result, &candidate))
            result->matches[result->count++] = candidate;
    }
}

soap_wsdd_mode wsdd_event_Resolve(struct soap* soap, const char* messageId, const char* replyTo,
                                  const char* endpointReference, struct wsdd__ResolveMatchType* match)
{
    (void)soap; (void)messageId; (void)replyTo; (void)endpointReference; (void)match;
    return SOAP_WSDD_ADHOC;
}

void wsdd_event_ResolveMatches(struct soap* soap, unsigned int instanceId, const char* sequenceId,
                               unsigned int messageNumber, const char* messageId,
                               const char* relatesTo, struct wsdd__ResolveMatchType* match)
{
    (void)soap; (void)instanceId; (void)sequenceId; (void)messageNumber; (void)messageId;
    (void)relatesTo; (void)match;
}

int onvif_wsdd_probe(unsigned int timeoutMs, OnvifWsddProbeResult* result)
{
    struct soap soap;
    unsigned int timeoutUs;
    int listenResult;
    const char* messageId;
    if (result == NULL)
        return -1;
    memset(result, 0, sizeof(*result));
    timeoutUs = timeoutMs == 0 ? ONVIF_WSDD_DEFAULT_TIMEOUT_US : timeoutMs * 1000U;
    if (timeoutUs > ONVIF_WSDD_MAX_TIMEOUT_US)
        timeoutUs = ONVIF_WSDD_MAX_TIMEOUT_US;

    soap_init1(&soap, SOAP_IO_UDP | SOAP_XML_TREE);
    soap.user = result;
    if (soap_register_plugin(&soap, soap_wsa) != SOAP_OK) {
        set_error(result, "注册 WS-Addressing 插件失败", &soap);
        soap_done(&soap);
        return -1;
    }
    if (!soap_valid_socket(soap_bind(&soap, NULL, 0, 16))) {
        set_error(result, "绑定 WS-Discovery 临时 UDP 端口失败", &soap);
        soap_done(&soap);
        return -1;
    }
    messageId = soap_wsa_rand_uuid(&soap);
    if (messageId == NULL || soap_wsdd_Probe(&soap, SOAP_WSDD_ADHOC, SOAP_WSDD_TO_TS,
                                             ONVIF_WSDD_ENDPOINT, messageId, NULL,
                                             ONVIF_WSDD_TYPES, NULL, NULL) != SOAP_OK) {
        set_error(result, "发送 WS-Discovery Probe 失败", &soap);
        soap_destroy(&soap);
        soap_end(&soap);
        soap_done(&soap);
        return -1;
    }
    listenResult = soap_wsdd_listen(&soap, -(int)timeoutUs);
    if (listenResult != SOAP_OK && soap.errnum != 0) {
        set_error(result, "等待 WS-Discovery ProbeMatch 失败", &soap);
        soap_destroy(&soap);
        soap_end(&soap);
        soap_done(&soap);
        return -1;
    }
    soap_destroy(&soap);
    soap_end(&soap);
    soap_done(&soap);
    return 0;
}
