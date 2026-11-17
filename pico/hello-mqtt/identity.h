/*
 * The Pico's identity: its private key, made on the Pico at first boot, the certificate and endpoint that
 * registration returns, and the Wi-Fi settings sent by pico-enrol.py. They live in the top three sectors of
 * flash, outside the program. With the key ladder, the key and the Wi-Fi settings are wrapped under the KEK.
 */
#ifndef IDENTITY_H
#define IDENTITY_H

#include <stdbool.h>
#include <stddef.h>

#include "mbedtls/pk.h"

#define THING_NAME_MAX 64
#define ENDPOINT_MAX 128
#define CERT_PEM_MAX 2048
#define WIFI_SSID_MAX 32
#define WIFI_PASSWORD_MAX 64

typedef struct {
    char endpoint[ENDPOINT_MAX];
    char cert_pem[CERT_PEM_MAX];
} identity_config_t;

typedef struct {
    char ssid[WIFI_SSID_MAX + 1];
    char password[WIFI_PASSWORD_MAX + 1];
} wifi_config_t;

/* Loads the key, or makes and stores one if there is none. Returns false on failure. */
bool identity_init(bool *created);

/* "rpi-ex-" and the board's unique ID, in lower case. */
const char *identity_thing_name(void);

/* The key, for TLS. */
mbedtls_pk_context *identity_key(void);

/* A PEM CSR for the key, with CN = the thing name. Returns 0 or an mbedTLS error. */
int identity_csr_pem(unsigned char *buf, size_t len);

/* Whether a PEM certificate holds this Pico's public key. */
bool identity_cert_matches(const char *cert_pem);

/* The saved certificate and endpoint, or NULL before registration. */
const identity_config_t *identity_config(void);
bool identity_config_save(const identity_config_t *config);

/* The saved Wi-Fi settings, or NULL if there are none (or, with the key ladder, no key). */
const wifi_config_t *identity_wifi(void);
bool identity_wifi_save(const wifi_config_t *wifi);

/* Forget the certificate and endpoint, and with forget_all the key and the Wi-Fi settings too. */
bool identity_reset(bool forget_all);

/* mbedTLS random generator, seeded from the hardware entropy source. */
int identity_random(void *ctx, unsigned char *out, size_t len);

#endif
