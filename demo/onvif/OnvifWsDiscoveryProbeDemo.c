#include "stdsoap2.h"
#include "wsddapi.h"

#include <stdio.h>

#define WSDD_MULTICAST_ENDPOINT "soap.udp://239.255.255.250:3702"
#define ONVIF_NETWORK_VIDEO_TRANSMITTER "dn:NetworkVideoTransmitter"

struct ProbeState {
    unsigned int match_count;
};

/* The WSDD plug-in links these six hooks. This client only consumes ProbeMatches. */
void wsdd_event_Hello(struct soap *soap, unsigned int instance_id, const char *sequence_id,
                      unsigned int message_number, const char *message_id, const char *relates_to,
                      const char *endpoint_reference, const char *types, const char *scopes,
                      const char *match_by, const char *xaddrs, unsigned int metadata_version)
{
    (void)soap; (void)instance_id; (void)sequence_id; (void)message_number; (void)message_id;
    (void)relates_to; (void)endpoint_reference; (void)types; (void)scopes; (void)match_by;
    (void)xaddrs; (void)metadata_version;
}

void wsdd_event_Bye(struct soap *soap, unsigned int instance_id, const char *sequence_id,
                    unsigned int message_number, const char *message_id, const char *relates_to,
                    const char *endpoint_reference, const char *types, const char *scopes,
                    const char *match_by, const char *xaddrs, unsigned int *metadata_version)
{
    (void)soap; (void)instance_id; (void)sequence_id; (void)message_number; (void)message_id;
    (void)relates_to; (void)endpoint_reference; (void)types; (void)scopes; (void)match_by;
    (void)xaddrs; (void)metadata_version;
}

soap_wsdd_mode wsdd_event_Probe(struct soap *soap, const char *message_id, const char *reply_to,
                                const char *types, const char *scopes, const char *match_by,
                                struct wsdd__ProbeMatchesType *matches)
{
    (void)soap; (void)message_id; (void)reply_to; (void)types; (void)scopes; (void)match_by;
    (void)matches;
    return SOAP_WSDD_ADHOC;
}

void wsdd_event_ProbeMatches(struct soap *soap, unsigned int instance_id, const char *sequence_id,
                             unsigned int message_number, const char *message_id,
                             const char *relates_to, struct wsdd__ProbeMatchesType *matches)
{
    struct ProbeState *state = (struct ProbeState *)soap->user;
    int index;
    (void)instance_id; (void)sequence_id; (void)message_number; (void)message_id; (void)relates_to;

    if (matches == NULL)
        return;

    for (index = 0; index < matches->__sizeProbeMatch; ++index) {
        const struct wsdd__ProbeMatchType *match = &matches->ProbeMatch[index];
        ++state->match_count;
        printf("match=%u endpoint=%s xaddr=%s types=%s\\n", state->match_count,
               match->wsa5__EndpointReference.Address ? match->wsa5__EndpointReference.Address : "<none>",
               match->XAddrs ? match->XAddrs : "<none>",
               match->Types ? match->Types : "<none>");
    }
}

soap_wsdd_mode wsdd_event_Resolve(struct soap *soap, const char *message_id, const char *reply_to,
                                  const char *endpoint_reference, struct wsdd__ResolveMatchType *match)
{
    (void)soap; (void)message_id; (void)reply_to; (void)endpoint_reference; (void)match;
    return SOAP_WSDD_ADHOC;
}

void wsdd_event_ResolveMatches(struct soap *soap, unsigned int instance_id, const char *sequence_id,
                               unsigned int message_number, const char *message_id,
                               const char *relates_to, struct wsdd__ResolveMatchType *match)
{
    (void)soap; (void)instance_id; (void)sequence_id; (void)message_number; (void)message_id;
    (void)relates_to; (void)match;
}

int main(void)
{
    struct soap soap;
    struct ProbeState state = { 0 };
    const char *message_id;
    int result;

    soap_init1(&soap, SOAP_IO_UDP | SOAP_XML_TREE);
    soap.user = &state;
    if (soap_register_plugin(&soap, soap_wsa) != SOAP_OK) {
        fprintf(stderr, "OnvifWsDiscoveryProbe: register WS-Addressing failed: soap=%d\\n", soap.error);
        soap_done(&soap);
        return 1;
    }

    /* Bind an ephemeral UDP port first: ProbeMatches is unicast back to this exact source port. */
    if (!soap_valid_socket(soap_bind(&soap, NULL, 0, 4))) {
        fprintf(stderr, "OnvifWsDiscoveryProbe: bind failed: soap=%d errno=%d\\n", soap.error, soap.errnum);
        soap_done(&soap);
        return 1;
    }

    message_id = soap_wsa_rand_uuid(&soap);
    result = soap_wsdd_Probe(&soap, SOAP_WSDD_ADHOC, SOAP_WSDD_TO_TS,
                             WSDD_MULTICAST_ENDPOINT, message_id, NULL,
                             ONVIF_NETWORK_VIDEO_TRANSMITTER, NULL, NULL);
    if (result != SOAP_OK) {
        fprintf(stderr, "OnvifWsDiscoveryProbe: send Probe failed: soap=%d errno=%d\\n", soap.error, soap.errnum);
        soap_done(&soap);
        return 1;
    }

    /* A Target Service may randomize its response delay; wait three seconds for all matches. */
    result = soap_wsdd_listen(&soap, -3000000);
    if (result != SOAP_OK && soap.errnum != 0)
        fprintf(stderr, "OnvifWsDiscoveryProbe: listen ended: soap=%d errno=%d\\n", soap.error, soap.errnum);

    printf("OnvifWsDiscoveryProbe: discovered=%u\\n", state.match_count);
    soap_destroy(&soap);
    soap_end(&soap);
    soap_done(&soap);
    return state.match_count > 0 ? 0 : 2;
}
