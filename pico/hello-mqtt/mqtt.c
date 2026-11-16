#include "mqtt.h"

#include <stdio.h>
#include <string.h>

#include "aws_root_ca.h"
#include "core_mqtt.h"
#include "identity.h"
#include "lwip/altcp_tls.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/ssl.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#define MQTT_PORT 8883
#define KEEP_ALIVE_S 60

/* coreMQTT asks for blocking send and receive; lwIP here runs from the Wi-Fi interrupt. Received data is
 * queued by the receive callback and handed to coreMQTT as it asks. Every lwIP call made from the main
 * loop is inside cyw43_arch_lwip_begin() and _end(). */
struct NetworkContext {
    struct altcp_pcb *pcb;
    struct pbuf *rx;
    volatile bool connected;
    volatile bool closed;
};

static struct NetworkContext net;
static struct altcp_tls_config *tls_config;
static MQTTContext_t mqtt;
static uint8_t mqtt_buffer[2048];
static MQTTPubAckInfo_t outgoing[4];
static MQTTPubAckInfo_t incoming[4];
static mqtt_message_fn message_fn;
static volatile bool suback_received, suback_ok;

static err_t on_recv(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)arg;
    (void)pcb;
    if (!p || err != ERR_OK) {
        if (p) {
            pbuf_free(p);
        }
        net.closed = true;
        return ERR_OK;
    }
    if (net.rx) {
        pbuf_cat(net.rx, p);
    } else {
        net.rx = p;
    }
    return ERR_OK;
}

static void on_err(void *arg, err_t err)
{
    (void)arg;
    printf("mqtt: connection error %d\n", err);
    net.pcb = NULL; /* lwIP has freed it */
    net.closed = true;
}

static err_t on_connected(void *arg, struct altcp_pcb *pcb, err_t err)
{
    (void)arg;
    (void)pcb;
    if (err == ERR_OK) {
        net.connected = true; /* for altcp_tls: after the TLS handshake */
    } else {
        net.closed = true;
    }
    return ERR_OK;
}

static int32_t transport_recv(NetworkContext_t *nc, void *buf, size_t len)
{
    int32_t n;
    cyw43_arch_lwip_begin();
    if (nc->rx) {
        u16_t want = len > 0xFFFF ? 0xFFFF : (u16_t)len;
        u16_t got = pbuf_copy_partial(nc->rx, buf, want, 0);
        nc->rx = pbuf_free_header(nc->rx, got);
        if (nc->pcb) {
            altcp_recved(nc->pcb, got);
        }
        n = got;
    } else {
        n = (nc->closed || !nc->pcb) ? -1 : 0; /* 0: nothing yet, ask again */
    }
    cyw43_arch_lwip_end();
    return n;
}

static int32_t transport_send(NetworkContext_t *nc, const void *buf, size_t len)
{
    int32_t n;
    cyw43_arch_lwip_begin();
    if (nc->closed || !nc->pcb) {
        n = -1;
    } else {
        u16_t room = altcp_sndbuf(nc->pcb);
        u16_t take = len < room ? (u16_t)len : room;
        if (take == 0) {
            n = 0;
        } else {
            err_t e = altcp_write(nc->pcb, buf, take, TCP_WRITE_FLAG_COPY);
            if (e == ERR_OK) {
                altcp_output(nc->pcb);
                n = take;
            } else {
                n = e == ERR_MEM ? 0 : -1;
            }
        }
    }
    cyw43_arch_lwip_end();
    return n;
}

static uint32_t now_ms(void)
{
    return to_ms_since_boot(get_absolute_time());
}

static void on_mqtt_event(MQTTContext_t *ctx, MQTTPacketInfo_t *packet, MQTTDeserializedInfo_t *info)
{
    (void)ctx;
    if ((packet->type & 0xF0u) == MQTT_PACKET_TYPE_PUBLISH) {
        const MQTTPublishInfo_t *pub = info->pPublishInfo;
        if (message_fn) {
            message_fn(pub->pTopicName, pub->topicNameLength, pub->pPayload, pub->payloadLength);
        }
    } else if (packet->type == MQTT_PACKET_TYPE_SUBACK) {
        uint8_t *codes;
        size_t n;
        suback_ok = MQTT_GetSubAckStatusCodes(packet, &codes, &n) == MQTTSuccess && n == 1 && codes[0] != 0x80;
        suback_received = true;
    }
}

typedef struct {
    volatile bool done;
    ip_addr_t addr;
    volatile bool ok;
} dns_result_t;

static void on_dns(const char *name, const ip_addr_t *addr, void *arg)
{
    (void)name;
    dns_result_t *r = arg;
    if (addr) {
        r->addr = *addr;
        r->ok = true;
    }
    r->done = true;
}

static void close_transport(void)
{
    cyw43_arch_lwip_begin();
    if (net.pcb) {
        altcp_arg(net.pcb, NULL);
        altcp_recv(net.pcb, NULL);
        altcp_err(net.pcb, NULL);
        if (altcp_close(net.pcb) != ERR_OK) {
            altcp_abort(net.pcb);
        }
    }
    if (net.rx) {
        pbuf_free(net.rx);
    }
    memset(&net, 0, sizeof net);
    cyw43_arch_lwip_end();
}

static int fail(int step)
{
    close_transport();
    return step;
}

int mqtt_connect(const char *endpoint, const char *client_id, const char *cert_pem, const char *subscribe_topic,
                 mqtt_message_fn on_message)
{
    message_fn = on_message;
    if (!tls_config) {
        /* The CA, this Pico's certificate and its key (as DER, from the identity). PEM lengths count the NUL. */
        unsigned char key_der[256];
        int n = mbedtls_pk_write_key_der(identity_key(), key_der, sizeof key_der);
        if (n <= 0) {
            return -2;
        }
        tls_config = altcp_tls_create_config_client_2wayauth(
            (const u8_t *)AWS_ROOT_CA_PEM, sizeof AWS_ROOT_CA_PEM, key_der + sizeof key_der - n, (size_t)n, NULL, 0,
            (const u8_t *)cert_pem, strlen(cert_pem) + 1);
        mbedtls_platform_zeroize(key_der, sizeof key_der);
        if (!tls_config) {
            return -2;
        }
    }

    static dns_result_t dns; /* static: a late callback must not write to a stack frame that is gone */
    dns.done = dns.ok = false;
    cyw43_arch_lwip_begin();
    err_t e = dns_gethostbyname(endpoint, &dns.addr, on_dns, &dns);
    cyw43_arch_lwip_end();
    if (e == ERR_OK) {
        dns.ok = dns.done = true;
    } else if (e != ERR_INPROGRESS) {
        return -1;
    }
    absolute_time_t deadline = make_timeout_time_ms(10000);
    while (!dns.done && !time_reached(deadline)) {
        sleep_ms(10);
    }
    if (!dns.ok) {
        return -1;
    }

    cyw43_arch_lwip_begin();
    memset(&net, 0, sizeof net);
    net.pcb = altcp_tls_new(tls_config, IP_GET_TYPE(&dns.addr));
    if (net.pcb) {
        mbedtls_ssl_set_hostname(altcp_tls_context(net.pcb), endpoint); /* SNI, and the server's name check */
        altcp_recv(net.pcb, on_recv);
        altcp_err(net.pcb, on_err);
        if (altcp_connect(net.pcb, &dns.addr, MQTT_PORT, on_connected) != ERR_OK) {
            net.closed = true;
        }
    }
    cyw43_arch_lwip_end();
    if (!net.pcb) {
        return fail(-2);
    }
    deadline = make_timeout_time_ms(30000);
    while (!net.connected && !net.closed && !time_reached(deadline)) {
        sleep_ms(10);
    }
    if (!net.connected) {
        return fail(-2); /* a wrong clock, CA or certificate, or a certificate AWS IoT does not accept */
    }

    TransportInterface_t transport = {.pNetworkContext = &net, .send = transport_send, .recv = transport_recv};
    MQTTFixedBuffer_t buffer = {mqtt_buffer, sizeof mqtt_buffer};
    memset(outgoing, 0, sizeof outgoing);
    memset(incoming, 0, sizeof incoming);
    if (MQTT_Init(&mqtt, &transport, now_ms, on_mqtt_event, &buffer) != MQTTSuccess ||
        MQTT_InitStatefulQoS(&mqtt, outgoing, 4, incoming, 4) != MQTTSuccess) {
        return fail(-3);
    }
    MQTTConnectInfo_t info = {
        .cleanSession = true,
        .pClientIdentifier = client_id,
        .clientIdentifierLength = (uint16_t)strlen(client_id),
        .keepAliveSeconds = KEEP_ALIVE_S,
    };
    bool session;
    MQTTStatus_t st = MQTT_Connect(&mqtt, &info, NULL, 10000, &session);
    if (st != MQTTSuccess) {
        printf("mqtt: CONNECT refused (%s): is the policy attached, and the client ID the thing name?\n",
               MQTT_Status_strerror(st));
        return fail(-3);
    }

    MQTTSubscribeInfo_t sub = {.qos = MQTTQoS1, .pTopicFilter = subscribe_topic,
                               .topicFilterLength = (uint16_t)strlen(subscribe_topic)};
    suback_received = suback_ok = false;
    st = MQTT_Subscribe(&mqtt, &sub, 1, MQTT_GetPacketId(&mqtt));
    while (st == MQTTSuccess && !suback_received && !time_reached(deadline)) {
        st = MQTT_ProcessLoop(&mqtt);
        if (st == MQTTNeedMoreBytes) {
            st = MQTTSuccess;
        }
        sleep_ms(5);
    }
    if (!suback_ok) {
        MQTT_Disconnect(&mqtt);
        return fail(-4);
    }
    return 0;
}

bool mqtt_publish(const char *topic, const char *payload, size_t len)
{
    MQTTPublishInfo_t pub = {.qos = MQTTQoS1, .pTopicName = topic, .topicNameLength = (uint16_t)strlen(topic),
                             .pPayload = payload, .payloadLength = len};
    MQTTStatus_t st = MQTT_Publish(&mqtt, &pub, MQTT_GetPacketId(&mqtt));
    if (st != MQTTSuccess) {
        printf("mqtt: publish failed (%s)\n", MQTT_Status_strerror(st));
    }
    return st == MQTTSuccess;
}

bool mqtt_poll(void)
{
    if (net.closed || !net.pcb) {
        return false;
    }
    MQTTStatus_t st = MQTT_ProcessLoop(&mqtt);
    if (st != MQTTSuccess && st != MQTTNeedMoreBytes) {
        printf("mqtt: connection lost (%s)\n", MQTT_Status_strerror(st));
        return false;
    }
    return true;
}

void mqtt_disconnect(void)
{
    if (mqtt.connectStatus == MQTTConnected) {
        MQTT_Disconnect(&mqtt);
    }
    close_transport();
}
