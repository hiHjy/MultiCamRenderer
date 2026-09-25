#pragma once

/*
 * gSOAP needs global C entry points for generated ONVIF operations.  This is
 * the small C boundary hidden behind the C++ OnvifServer; application code
 * should include OnvifServer.hpp instead of this header.
 */

#ifdef __cplusplus
extern "C" {
#endif

enum {
    ONVIF_SOAP_URL_CAPACITY = 256,
    ONVIF_SOAP_TEXT_CAPACITY = 96,
    ONVIF_SOAP_ENDPOINT_CAPACITY = 128,
    ONVIF_SOAP_SCOPES_CAPACITY = 384,
};

typedef struct OnvifSoapServiceConfig {
    char deviceServiceUrl[ONVIF_SOAP_URL_CAPACITY];
    char mediaServiceUrl[ONVIF_SOAP_URL_CAPACITY];
    char media2ServiceUrl[ONVIF_SOAP_URL_CAPACITY];
    char mainRtspUrl[ONVIF_SOAP_URL_CAPACITY];
    char subRtspUrl[ONVIF_SOAP_URL_CAPACITY];
    char endpointReference[ONVIF_SOAP_ENDPOINT_CAPACITY];
    char manufacturer[ONVIF_SOAP_TEXT_CAPACITY];
    char model[ONVIF_SOAP_TEXT_CAPACITY];
    char firmwareVersion[ONVIF_SOAP_TEXT_CAPACITY];
    char serialNumber[ONVIF_SOAP_TEXT_CAPACITY];
    char hardwareId[ONVIF_SOAP_TEXT_CAPACITY];
    /* Space-separated ONVIF scope URIs, shared by GetScopes and Discovery. */
    char scopes[ONVIF_SOAP_SCOPES_CAPACITY];
} OnvifSoapServiceConfig;

typedef int (*OnvifSoapStopRequestedFn)(void *context);
typedef void (*OnvifSoapStartedFn)(void *context, int success, const char *errorText);

/* These calls block until stopRequested returns non-zero. They report whether
 * the listening socket was successfully created through onStarted. */
int onvif_soap_run_device_service(const OnvifSoapServiceConfig *config,
                                  unsigned short port,
                                  OnvifSoapStopRequestedFn stopRequested,
                                  void *stopContext,
                                  OnvifSoapStartedFn onStarted,
                                  void *startedContext);

int onvif_soap_run_discovery_service(const OnvifSoapServiceConfig *config,
                                     OnvifSoapStopRequestedFn stopRequested,
                                     void *stopContext,
                                     OnvifSoapStartedFn onStarted,
                                     void *startedContext);

#ifdef __cplusplus
}
#endif
