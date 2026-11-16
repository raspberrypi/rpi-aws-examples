/*
 * hello-mqtt: a Pico 2 W (or Pico W) that sends "hello" and then a count every 10 seconds to AWS IoT Core.
 *
 * First boot: the Pico makes its own private key and keeps it in flash. It never leaves the Pico. You
 * register it from your computer with pico/tools/pico-enrol.py, which uses these commands over USB serial
 * (one per line; each answer ends with "OK" or "ERROR <reason>"):
 *
 *   ID                 the thing name: rpi-ex-<board id>
 *   CSR                a certificate signing request for the key (PEM)
 *   ENDPOINT <host>    the AWS IoT endpoint, from aws-iot-register
 *   CERT               then the certificate's PEM lines, up to "-----END CERTIFICATE-----"
 *   SAVE               check the certificate is for this key, save it and the endpoint, restart
 *   STATUS             registered or not
 *   WIFI-SSID <name>   the Wi-Fi network (WPA2), then
 *   WIFI-PASSWORD <p>  its password (8-63 characters, or 64 hex digits), then
 *   WIFI-SAVE          save them (wrapped, with the key ladder); restart if it was running
 *   WIFI-STATUS        the saved network's name, or "none"; never the password
 *   RESET [KEY]        forget the certificate and endpoint (and with KEY the key and Wi-Fi too), restart
 *
 * Built with -DHELLO_KEY_LADDER=ON (Pico 2 W), the key is wrapped under a root key in OTP (ladder.h), and:
 *   OTP-STATUS         the root's state (blank, programmed, interrupted), its page and rows, the stepping
 *   OTP-INIT <thing>   WRITE THE ROOT TO OTP: PERMANENT. Only on a blank page, and only with this Pico's
 *                      thing name as confirmation. pico/tools/pico-otp-init.py sends it, after its dry run.
 *
 * Once registered and given Wi-Fi settings it joins Wi-Fi, sets its clock with SNTP, connects with client ID =
 * thing name, publishes {"hello": ...} on rpi/<thing>/hello once, then {"count": n} on rpi/<thing>/count
 * every 10 s, and prints what arrives on rpi/<thing>/cmd.
 */
#include <stdio.h>
#include <string.h>

#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "identity.h"
#if HELLO_KEY_LADDER
#include "ladder.h"
#endif
#include "lwip/apps/sntp.h"
#include "mbedtls/platform.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/platform_time.h"
#include "mqtt.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#define COUNT_INTERVAL_MS 10000
#define TOPIC_MAX (THING_NAME_MAX + 16)

/* ---- clock: SNTP sets it; TLS needs it to check certificate dates ---- */

static int64_t epoch_offset_us; /* wall-clock microseconds minus time_us_64() */
static volatile bool clock_set;

void hello_set_clock(unsigned long sec, unsigned long us)
{
    uint32_t irq = save_and_disable_interrupts();
    epoch_offset_us = (int64_t)sec * 1000000 + (int64_t)us - (int64_t)time_us_64();
    clock_set = true;
    restore_interrupts(irq);
}

static mbedtls_time_t tls_time(mbedtls_time_t *t)
{
    uint32_t irq = save_and_disable_interrupts();
    int64_t offset = epoch_offset_us;
    restore_interrupts(irq);
    mbedtls_time_t now = clock_set ? (mbedtls_time_t)((offset + (int64_t)time_us_64()) / 1000000) : 0;
    if (t) {
        *t = now;
    }
    return now;
}

mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)(time_us_64() / 1000);
}

/* ---- USB serial commands ---- */

static identity_config_t pending; /* what ENDPOINT and CERT have received, until SAVE */
static wifi_config_t pending_wifi; /* what WIFI-SSID and WIFI-PASSWORD have received, until WIFI-SAVE */
static bool in_cert;
static bool have_key;
static bool running; /* in run(): new settings take a restart */
#if HELLO_KEY_LADDER
static ladder_state_t otp_state; /* read at boot, before the page is locked */
#endif

static void restart(void)
{
    stdio_flush();
    sleep_ms(100);
    watchdog_reboot(0, 0, 0);
    while (true) {
        tight_loop_contents();
    }
}

static void command(char *line)
{
    if (in_cert) {
        size_t used = strlen(pending.cert_pem);
        if (used + strlen(line) + 2 > sizeof pending.cert_pem) {
            in_cert = false;
            pending.cert_pem[0] = '\0';
            printf("ERROR certificate too long\n");
            return;
        }
        strcat(pending.cert_pem, line);
        strcat(pending.cert_pem, "\n");
        if (strcmp(line, "-----END CERTIFICATE-----") == 0) {
            in_cert = false;
            printf("OK\n");
        }
        return;
    }
#if HELLO_KEY_LADDER
    static const char *state_names[] = {"blank", "programmed", "interrupted"};
    if (strcmp(line, "OTP-STATUS") == 0) {
        printf("otp-root: %s page %u rows 0x%03x-0x%03x stepping A%u\nOK\n", state_names[otp_state], ladder_page(),
               ladder_first_row(), ladder_first_row() + 16, ladder_stepping());
        return;
    }
    if (strncmp(line, "OTP-INIT ", 9) == 0) {
        if (strcmp(line + 9, identity_thing_name()) != 0) {
            printf("ERROR confirm with this Pico's thing name\n");
        } else if (otp_state != LADDER_BLANK) {
            printf("ERROR the root is %s; nothing written\n", state_names[otp_state]);
        } else {
            int rc = ladder_write_root();
            printf(rc == 0 ? "OK\n" : "ERROR OTP write failed (%d)\n", rc);
            restart(); /* the next boot reads the root, makes the key and locks the page */
        }
        return;
    }
#endif
    if (!have_key && (strcmp(line, "CSR") == 0 || strcmp(line, "SAVE") == 0 || strcmp(line, "WIFI-SAVE") == 0)) {
        printf("ERROR this Pico has no key\n");
        return;
    }
    if (strcmp(line, "ID") == 0) {
        printf("%s\nOK\n", identity_thing_name());
    } else if (strcmp(line, "CSR") == 0) {
        static unsigned char pem[1024];
        int rc = identity_csr_pem(pem, sizeof pem);
        if (rc == 0) {
            printf("%sOK\n", pem);
        } else {
            printf("ERROR csr %d\n", rc);
        }
    } else if (strncmp(line, "ENDPOINT ", 9) == 0 && strlen(line + 9) < sizeof pending.endpoint) {
        strcpy(pending.endpoint, line + 9);
        printf("OK\n");
    } else if (strcmp(line, "CERT") == 0) {
        pending.cert_pem[0] = '\0';
        in_cert = true;
    } else if (strcmp(line, "SAVE") == 0) {
        if (!pending.endpoint[0] || !pending.cert_pem[0]) {
            printf("ERROR send ENDPOINT and CERT first\n");
        } else if (!identity_cert_matches(pending.cert_pem)) {
            printf("ERROR the certificate is not for this Pico's key\n");
        } else if (!identity_config_save(&pending)) {
            printf("ERROR flash write failed\n");
        } else {
            printf("OK\n");
            restart();
        }
    } else if (strncmp(line, "WIFI-SSID ", 10) == 0 && line[10] && strlen(line + 10) <= WIFI_SSID_MAX) {
        strcpy(pending_wifi.ssid, line + 10);
        printf("OK\n");
    } else if (strncmp(line, "WIFI-PASSWORD ", 14) == 0 && strlen(line + 14) >= 8 &&
               strlen(line + 14) <= WIFI_PASSWORD_MAX) {
        strcpy(pending_wifi.password, line + 14);
        mbedtls_platform_zeroize(line, strlen(line));
        printf("OK\n");
    } else if (strcmp(line, "WIFI-SAVE") == 0) {
        bool ok = pending_wifi.ssid[0] && pending_wifi.password[0] && identity_wifi_save(&pending_wifi);
        mbedtls_platform_zeroize(&pending_wifi, sizeof pending_wifi);
        printf(ok ? "OK\n" : "ERROR send WIFI-SSID and WIFI-PASSWORD first, or flash write failed\n");
        if (ok && running) {
            restart();
        }
    } else if (strcmp(line, "WIFI-STATUS") == 0) {
        const wifi_config_t *w = identity_wifi();
        printf("wifi: %s\nOK\n", w ? w->ssid : "none");
    } else if (strcmp(line, "STATUS") == 0) {
        const identity_config_t *c = identity_config();
        printf("%s %s\nOK\n", identity_thing_name(), c ? c->endpoint : "unregistered");
    } else if (strcmp(line, "RESET") == 0 || strcmp(line, "RESET KEY") == 0) {
        bool ok = identity_reset(strcmp(line, "RESET KEY") == 0);
        printf(ok ? "OK\n" : "ERROR flash write failed\n");
        restart();
    } else if (line[0]) {
        printf("ERROR unknown command\n");
    }
}

/* Reads whatever has arrived, without waiting, and runs each complete line. */
static void poll_serial(void)
{
    static char line[160];
    static size_t len;
    int c;
    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            line[len] = '\0';
            command(line);
            stdio_flush(); /* each answer goes out whole, before anything else is read */
            len = 0;
        } else if (len < sizeof line - 1) {
            line[len++] = (char)c;
        }
    }
}

/* ---- MQTT ---- */

static void on_message(const char *topic, size_t topic_len, const char *payload, size_t len)
{
    printf("message on %.*s: %.*s\n", (int)topic_len, topic, (int)len, payload);
}

static void wait_ms(uint32_t ms)
{
    absolute_time_t until = make_timeout_time_ms(ms);
    while (!time_reached(until)) {
        poll_serial();
        sleep_ms(20);
    }
}

static void run(const identity_config_t *config, const wifi_config_t *wifi)
{
    running = true;
    const char *thing = identity_thing_name();
    char topic_hello[TOPIC_MAX], topic_count[TOPIC_MAX], topic_cmd[TOPIC_MAX], payload[96];
    snprintf(topic_hello, sizeof topic_hello, "rpi/%s/hello", thing);
    snprintf(topic_count, sizeof topic_count, "rpi/%s/count", thing);
    snprintf(topic_cmd, sizeof topic_cmd, "rpi/%s/cmd", thing);

    cyw43_arch_enable_sta_mode();
    printf("wifi: joining %s\n", wifi->ssid);
    while (cyw43_arch_wifi_connect_timeout_ms(wifi->ssid, wifi->password, CYW43_AUTH_WPA2_AES_PSK, 30000) != 0) {
        printf("wifi: failed, retrying\n");
        wait_ms(5000);
    }
    printf("wifi: connected\n");

    cyw43_arch_lwip_begin();
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_setservername(1, "time.aws.com");
    sntp_init();
    cyw43_arch_lwip_end();
    while (!clock_set) {
        wait_ms(100);
    }
    printf("clock: set by SNTP\n");

    uint32_t backoff_ms = 1000;
    unsigned count = 0;
    while (true) {
        int rc = mqtt_connect(config->endpoint, thing, config->cert_pem, topic_cmd, on_message);
        if (rc != 0) {
            static const char *steps[] = {"", "DNS", "TLS", "MQTT CONNECT", "SUBSCRIBE"};
            printf("mqtt: %s failed; retrying in %lu s\n", steps[-rc], (unsigned long)(backoff_ms / 1000));
            wait_ms(backoff_ms);
            backoff_ms = backoff_ms * 2 > 300000 ? 300000 : backoff_ms * 2;
            continue;
        }
        backoff_ms = 1000;
        printf("mqtt: connected to %s as %s, subscribed to %s\n", config->endpoint, thing, topic_cmd);
        if (count == 0) {
            int n = snprintf(payload, sizeof payload, "{\"hello\": \"%s\"}", thing);
            mqtt_publish(topic_hello, payload, (size_t)n);
            printf("publish %s %s\n", topic_hello, payload);
        }
        absolute_time_t next = get_absolute_time();
        while (mqtt_poll()) {
            if (time_reached(next)) {
                int n = snprintf(payload, sizeof payload, "{\"count\": %u}", ++count);
                if (mqtt_publish(topic_count, payload, (size_t)n)) {
                    printf("publish %s %s\n", topic_count, payload);
                }
                next = make_timeout_time_ms(COUNT_INTERVAL_MS);
            }
            poll_serial();
            sleep_ms(20);
        }
        mqtt_disconnect();
        wait_ms(backoff_ms);
    }
}

int main(void)
{
    stdio_init_all();
    sleep_ms(1000);
    printf("\nhello-mqtt\n");

    bool created = false;
#if HELLO_KEY_LADDER
    otp_state = ladder_state();
#endif
    have_key = identity_init(&created);
    if (!have_key) {
        printf("error: no key, so not connecting; commands still work\n");
        while (true) {
            poll_serial();
            sleep_ms(20);
        }
    }
    printf("key: %s (EC P-256, in flash)\nthing: %s\n", created ? "made now" : "loaded", identity_thing_name());

    if (cyw43_arch_init() != 0) {
        printf("error: Wi-Fi chip did not start\n");
        return 1;
    }
    mbedtls_platform_set_time(tls_time);

    const identity_config_t *config = identity_config();
    if (config && !identity_cert_matches(config->cert_pem)) {
        printf("the saved certificate is for another key (a new key was made): register again\n");
        config = NULL;
    }
    if (!config) {
        printf("not registered: run pico/tools/pico-enrol.py on your computer\n");
    } else if (!identity_wifi()) {
        printf("no Wi-Fi settings: run pico/tools/pico-enrol.py wifi on your computer\n");
    }
    /* Registration restarts the Pico; Wi-Fi settings saved now are used at once. */
    while (!config || !identity_wifi()) {
        poll_serial();
        sleep_ms(20);
    }
    run(config, identity_wifi());
}
