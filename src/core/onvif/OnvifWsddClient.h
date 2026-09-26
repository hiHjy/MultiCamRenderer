#pragma once

/* Private C bridge: gSOAP's wsddapi requires process-wide C callback symbols.
 * Keep that constraint here, then let the public C++ client expose std::string
 * and std::vector without leaking gSOAP types into application code. */

#ifdef __cplusplus
extern "C" {
#endif

enum {
    ONVIF_WSDD_MAX_MATCHES = 32,
    ONVIF_WSDD_EPR_CAPACITY = 128,
    ONVIF_WSDD_URL_CAPACITY = 256,
    ONVIF_WSDD_TYPES_CAPACITY = 128,
    ONVIF_WSDD_SCOPES_CAPACITY = 512,
    ONVIF_WSDD_ERROR_CAPACITY = 160,
};

typedef struct OnvifWsddProbeMatch {
    char endpointReference[ONVIF_WSDD_EPR_CAPACITY];
    char deviceServiceUrl[ONVIF_WSDD_URL_CAPACITY];
    char types[ONVIF_WSDD_TYPES_CAPACITY];
    char scopes[ONVIF_WSDD_SCOPES_CAPACITY];
} OnvifWsddProbeMatch;

typedef struct OnvifWsddProbeResult {
    unsigned int count;
    OnvifWsddProbeMatch matches[ONVIF_WSDD_MAX_MATCHES];
    char error[ONVIF_WSDD_ERROR_CAPACITY];
} OnvifWsddProbeResult;

/* Returns 0 when Probe was sent and the listening interval completed (including
 * an ordinary no-device timeout); returns -1 for socket/plugin/protocol errors. */
int onvif_wsdd_probe(unsigned int timeoutMs, OnvifWsddProbeResult* result);

#ifdef __cplusplus
}
#endif
