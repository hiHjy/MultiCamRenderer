#include "stdsoap2.h"
#include "wsddapi.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#define WSDD_PORT 3702
#define WSDD_MULTICAST_ADDRESS "239.255.255.250"
#define ONVIF_NETWORK_VIDEO_TRANSMITTER "dn:NetworkVideoTransmitter"

static volatile sig_atomic_t g_stop_requested = 0;

struct DiscoveryDeviceInfo {
    const char *endpoint_reference;
    const char *scopes;
    char device_service_url[256];
    unsigned int metadata_version;
};

static void on_signal(int signal_number)
{
    (void)signal_number;
    g_stop_requested = 1;
}

static int probe_requests_this_device_type(const char *types)
{
    /* 空 Types 代表“发现所有设备”；ONVIF 客户端通常请求 dn:NetworkVideoTransmitter。 */
    return types == NULL || *types == '\0'
        || strstr(types, "NetworkVideoTransmitter") != NULL;
}

static int get_peer_udp_endpoint(const struct soap *soap, char *endpoint, size_t endpoint_size)
{
    char host[NI_MAXHOST] = { 0 };
    char service[NI_MAXSERV] = { 0 };
    if (getnameinfo((const struct sockaddr *)&soap->peer, soap->peerlen,
                    host, sizeof(host), service, sizeof(service),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return -1;
    }

    const int written = snprintf(endpoint, endpoint_size, "soap.udp://%s:%s", host, service);
    return written > 0 && (size_t)written < endpoint_size ? 0 : -1;
}

/*
 * 以下六个函数是 gSOAP WSDD 插件要求应用提供的事件入口。
 *
 * 这个 demo 只实现“作为 ONVIF Target Service 收到 Probe 后返回 ProbeMatches”。
 * Hello/Bye/Resolve 及“作为客户端收到 Match”的逻辑暂时保留空实现；后续 C++ OnvifServer
 * 会持有这套 C binding，而不是复制一套 XML/UDP 协议代码。
 */
void wsdd_event_Hello(struct soap *soap, unsigned int instance_id, const char *sequence_id,
                      unsigned int message_number, const char *message_id,
                      const char *relates_to, const char *endpoint_reference,
                      const char *types, const char *scopes, const char *match_by,
                      const char *xaddrs, unsigned int metadata_version)
{
    (void)soap;
    (void)instance_id;
    (void)sequence_id;
    (void)message_number;
    (void)message_id;
    (void)relates_to;
    (void)endpoint_reference;
    (void)types;
    (void)scopes;
    (void)match_by;
    (void)xaddrs;
    (void)metadata_version;
}

void wsdd_event_Bye(struct soap *soap, unsigned int instance_id, const char *sequence_id,
                    unsigned int message_number, const char *message_id,
                    const char *relates_to, const char *endpoint_reference,
                    const char *types, const char *scopes, const char *match_by,
                    const char *xaddrs, unsigned int *metadata_version)
{
    (void)soap;
    (void)instance_id;
    (void)sequence_id;
    (void)message_number;
    (void)message_id;
    (void)relates_to;
    (void)endpoint_reference;
    (void)types;
    (void)scopes;
    (void)match_by;
    (void)xaddrs;
    (void)metadata_version;
}

soap_wsdd_mode wsdd_event_Probe(struct soap *soap, const char *message_id,
                                const char *reply_to, const char *types,
                                const char *scopes, const char *match_by,
                                struct wsdd__ProbeMatchesType *matches)
{
    struct DiscoveryDeviceInfo *device = (struct DiscoveryDeviceInfo *)soap->user;
    char reply_endpoint[NI_MAXHOST + NI_MAXSERV + 16] = { 0 };
    (void)reply_to;
    (void)scopes;
    (void)match_by;

    if (device == NULL || message_id == NULL || !probe_requests_this_device_type(types)) {
        /* Ad-hoc 模式下返回空 Match 即是不匹配；不能错误回复其它设备类型的 Probe。 */
        return SOAP_WSDD_ADHOC;
    }

    soap_wsdd_init_ProbeMatches(soap, matches);
    if (soap_wsdd_add_ProbeMatch(soap, matches,
                                 device->endpoint_reference,
                                 ONVIF_NETWORK_VIDEO_TRANSMITTER,
                                 device->scopes,
                                 NULL,
                                 device->device_service_url,
                                 device->metadata_version) != SOAP_OK) {
        fprintf(stderr, "OnvifWsDiscovery: construct ProbeMatch failed: soap=%d\n", soap->error);
        return SOAP_WSDD_ADHOC;
    }

    /* WS-Discovery ad-hoc Probe 的回复必须 UDP 单播回 Probe 源端口，不能回发组播组。 */
    if (get_peer_udp_endpoint(soap, reply_endpoint, sizeof(reply_endpoint)) != 0) {
        fprintf(stderr, "OnvifWsDiscovery: cannot determine Probe peer endpoint\n");
        return SOAP_WSDD_ADHOC;
    }

    if (soap_wsdd_ProbeMatches(soap, reply_endpoint, soap_wsa_rand_uuid(soap),
                               message_id, soap_wsa_anonymousURI, matches) != SOAP_OK) {
        fprintf(stderr, "OnvifWsDiscovery: send ProbeMatches failed: soap=%d\n", soap->error);
        return SOAP_WSDD_ADHOC;
    }

    fprintf(stdout, "OnvifWsDiscovery: replied Probe types=%s peer=%s xaddr=%s\n",
            types ? types : "<all>", reply_endpoint, device->device_service_url);
    return SOAP_WSDD_ADHOC;
}

void wsdd_event_ProbeMatches(struct soap *soap, unsigned int instance_id, const char *sequence_id,
                             unsigned int message_number, const char *message_id,
                             const char *relates_to, struct wsdd__ProbeMatchesType *matches)
{
    (void)soap;
    (void)instance_id;
    (void)sequence_id;
    (void)message_number;
    (void)message_id;
    (void)relates_to;
    (void)matches;
}

soap_wsdd_mode wsdd_event_Resolve(struct soap *soap, const char *message_id,
                                  const char *reply_to, const char *endpoint_reference,
                                  struct wsdd__ResolveMatchType *match)
{
    (void)soap;
    (void)message_id;
    (void)reply_to;
    (void)endpoint_reference;
    (void)match;
    return SOAP_WSDD_ADHOC;
}

void wsdd_event_ResolveMatches(struct soap *soap, unsigned int instance_id, const char *sequence_id,
                               unsigned int message_number, const char *message_id,
                               const char *relates_to, struct wsdd__ResolveMatchType *match)
{
    (void)soap;
    (void)instance_id;
    (void)sequence_id;
    (void)message_number;
    (void)message_id;
    (void)relates_to;
    (void)match;
}

int main(int argc, char **argv)
{
    struct DiscoveryDeviceInfo device = {
        "urn:uuid:2a07ced5-47fd-4a17-a036-6d215a95e111",
        "onvif://www.onvif.org/type/Network_Video_Transmitter "
        "onvif://www.onvif.org/name/MultiCamRenderer "
        "onvif://www.onvif.org/hardware/MultiCamRenderer",
        { 0 },
        1,
    };
    struct soap soap;
    struct ip_mreq membership;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <local-ipv4-address>\n", argv[0]);
        return 1;
    }

    if (snprintf(device.device_service_url, sizeof(device.device_service_url),
                 "http://%s/onvif/device_service", argv[1]) >= (int)sizeof(device.device_service_url)) {
        fprintf(stderr, "OnvifWsDiscovery: device service URL is too long\n");
        return 1;
    }

    /* stdout is redirected to a board-side log in normal use; retain every Probe log line. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    soap_init1(&soap, SOAP_IO_UDP | SOAP_XML_TREE);
    soap.user = &device;
    if (soap_register_plugin(&soap, soap_wsa) != SOAP_OK) {
        fprintf(stderr, "OnvifWsDiscovery: register WS-Addressing failed: soap=%d\n", soap.error);
        soap_done(&soap);
        return 1;
    }

    if (!soap_valid_socket(soap_bind(&soap, NULL, WSDD_PORT, 16))) {
        fprintf(stderr, "OnvifWsDiscovery: bind UDP %d failed: soap=%d errno=%d\n",
                WSDD_PORT, soap.error, soap.errnum);
        soap_done(&soap);
        return 1;
    }

    memset(&membership, 0, sizeof(membership));
    membership.imr_multiaddr.s_addr = inet_addr(WSDD_MULTICAST_ADDRESS);
    membership.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(soap.master, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   &membership, sizeof(membership)) != 0) {
        fprintf(stderr, "OnvifWsDiscovery: join multicast group failed: %s\n", strerror(errno));
        soap_done(&soap);
        return 1;
    }

    fprintf(stdout, "OnvifWsDiscovery: listening UDP %d group=%s xaddr=%s; Ctrl+C to stop\n",
            WSDD_PORT, WSDD_MULTICAST_ADDRESS, device.device_service_url);
    while (!g_stop_requested) {
        /* 200ms 超时让 Ctrl+C 能及时退出；每次 timeout 后 gSOAP 清理本次请求临时对象。 */
        const int result = soap_wsdd_listen(&soap, -200000);
        if (result != SOAP_OK && !g_stop_requested) {
            fprintf(stderr, "OnvifWsDiscovery: receive error soap=%d errno=%d\n", soap.error, soap.errnum);
        }
    }

    soap_destroy(&soap);
    soap_end(&soap);
    soap_done(&soap);
    fprintf(stdout, "OnvifWsDiscovery: stopped\n");
    return 0;
}
