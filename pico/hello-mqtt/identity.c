#include "identity.h"

#if HELLO_KEY_LADDER
#include "ladder.h"
#endif

#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecp.h"
#include "mbedtls/entropy.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/x509_csr.h"
#include "pico/flash.h"
#include "pico/unique_id.h"

/* The top sector holds the key, the one below it the certificate and endpoint, the next the Wi-Fi settings.
 * A program that grows into them is refused at boot (identity_init). Erase them with "picotool erase" or
 * the RESET command. */
#define KEY_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define CONFIG_OFFSET (PICO_FLASH_SIZE_BYTES - 2 * FLASH_SECTOR_SIZE)
#define WIFI_OFFSET (PICO_FLASH_SIZE_BYTES - 3 * FLASH_SECTOR_SIZE)

#define MAGIC_KEY 0x59454b52u    /* "RKEY": the key in plain DER (Pico W, or HELLO_KEY_LADDER=OFF) */
#define MAGIC_WRAPPED 0x4b575252u /* "RRWK": the key wrapped by the key ladder */
#define MAGIC_CONFIG 0x47464352u /* "RCFG" */
#define MAGIC_WIFI 0x49465752u   /* "RWFI": the Wi-Fi settings in plain (Pico W, or HELLO_KEY_LADDER=OFF) */
#define MAGIC_WIFI_WRAPPED 0x46575752u /* "RWWF": the Wi-Fi settings wrapped by the key ladder */

/* One record per sector: magic, length, CRC-32 of the data, then the data. Erased flash reads as absent. */
typedef struct {
    uint32_t magic;
    uint32_t len;
    uint32_t crc;
} record_header_t;

static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;
static mbedtls_pk_context key;
static char thing_name[THING_NAME_MAX];
static wifi_config_t wifi;
static bool have_wifi;
#if HELLO_KEY_LADDER
/* Kept for this boot, to wrap Wi-Fi settings sent later: the OTP page is locked once it is derived. It opens
 * no more than the plain key, which is in RAM anyway. */
static uint8_t kek[32];
#endif

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) {
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
        }
    }
    return ~crc;
}

static const uint8_t *record_read(uint32_t offset, uint32_t magic, size_t *len)
{
    const record_header_t *h = (const record_header_t *)(XIP_BASE + offset);
    const uint8_t *data = (const uint8_t *)(h + 1);
    if (h->magic != magic || h->len == 0 || h->len > FLASH_SECTOR_SIZE - sizeof *h || crc32(data, h->len) != h->crc) {
        return NULL;
    }
    *len = h->len;
    return data;
}

typedef struct {
    uint32_t offset;
    const uint8_t *page_data; /* NULL: erase only */
    size_t len;
} flash_job_t;

/* Runs with interrupts off (flash_safe_execute): nothing may run from flash during an erase. */
static void __no_inline_not_in_flash_func(flash_job)(void *arg)
{
    const flash_job_t *job = arg;
    flash_range_erase(job->offset, FLASH_SECTOR_SIZE);
    if (job->page_data) {
        flash_range_program(job->offset, job->page_data, job->len);
    }
}

static bool record_write(uint32_t offset, uint32_t magic, const void *data, size_t len)
{
    static uint8_t sector[FLASH_SECTOR_SIZE];
    flash_job_t job = {offset, NULL, 0};
    if (data) {
        record_header_t h = {magic, (uint32_t)len, crc32(data, len)};
        if (len > sizeof sector - sizeof h) {
            return false;
        }
        memset(sector, 0xFF, sizeof sector);
        memcpy(sector, &h, sizeof h);
        memcpy(sector + sizeof h, data, len);
        job.page_data = sector;
        job.len = (sizeof h + len + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE;
    }
    bool ok = flash_safe_execute(flash_job, &job, 1000) == PICO_OK;
    mbedtls_platform_zeroize(sector, sizeof sector); /* it may have held the key */
    return ok;
}

int identity_random(void *ctx, unsigned char *out, size_t len)
{
    (void)ctx;
    return mbedtls_ctr_drbg_random(&drbg, out, len);
}

/* Loads the Wi-Fi settings into RAM. With the key ladder they are unwrapped; plain ones left by a plain-key
 * build are wrapped and written back over, so that they are not left plain in flash. */
static void wifi_load(void)
{
    size_t len;
#if HELLO_KEY_LADDER
    const uint8_t *data = record_read(WIFI_OFFSET, MAGIC_WIFI_WRAPPED, &len);
    size_t plain_len;
    if (data) {
        have_wifi = ladder_unwrap(kek, data, len, (uint8_t *)&wifi, sizeof wifi, &plain_len) == 0 &&
                    plain_len == sizeof wifi;
    } else if ((data = record_read(WIFI_OFFSET, MAGIC_WIFI, &len)) != NULL && len == sizeof wifi) {
        memcpy(&wifi, data, sizeof wifi);
        have_wifi = identity_wifi_save(&wifi);
    }
#else
    const uint8_t *data = record_read(WIFI_OFFSET, MAGIC_WIFI, &len);
    if (data && len == sizeof wifi) {
        memcpy(&wifi, data, sizeof wifi);
        have_wifi = true;
    }
#endif
    wifi.ssid[WIFI_SSID_MAX] = wifi.password[WIFI_PASSWORD_MAX] = '\0';
    if (!have_wifi) {
        mbedtls_platform_zeroize(&wifi, sizeof wifi);
    }
}

static bool key_init(bool *created);

bool identity_init(bool *created)
{
    bool ok = key_init(created);
    if (ok) {
        wifi_load();
    }
    return ok;
}

static bool key_init(bool *created)
{
    extern char __flash_binary_end;
    if ((uintptr_t)&__flash_binary_end - XIP_BASE > WIFI_OFFSET) {
        printf("error: the program has grown into the identity sectors\n");
        return false;
    }

    char id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    pico_get_unique_board_id_string(id, sizeof id);
    for (char *c = id; *c; c++) {
        if (*c >= 'A' && *c <= 'Z') {
            *c = (char)(*c - 'A' + 'a');
        }
    }
    snprintf(thing_name, sizeof thing_name, "rpi-ex-%s", id);

    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_pk_init(&key);
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)thing_name,
                              strlen(thing_name)) != 0) {
        printf("error: no entropy\n");
        return false;
    }

    size_t len;
    static unsigned char buf[256];
#if HELLO_KEY_LADDER
    /* The key ladder: no root, no key. Never a plain key instead. */
    if (ladder_state() != LADDER_PROGRAMMED || ladder_kek(kek) != 0) {
        printf("key ladder: no OTP root on this Pico, so no key; see pico/tools/pico-otp-init.py\n");
        return false;
    }
    const uint8_t *blob = record_read(KEY_OFFSET, MAGIC_WRAPPED, &len);
    *created = blob == NULL;
    if (blob) {
        size_t der_len;
        bool ok = ladder_unwrap(kek, blob, len, buf, sizeof buf, &der_len) == 0 &&
                  mbedtls_pk_parse_key(&key, buf, der_len, NULL, 0, identity_random, NULL) == 0;
        mbedtls_platform_zeroize(buf, sizeof buf);
        if (!ok) {
            printf("key ladder: the wrapped key does not open on this Pico (another Pico's, or damaged)\n");
        }
        return ok;
    }
#else
    const uint8_t *der = record_read(KEY_OFFSET, MAGIC_KEY, &len);
    *created = der == NULL;
    if (der) {
        return mbedtls_pk_parse_key(&key, der, len, NULL, 0, identity_random, NULL) == 0;
    }
#endif

    /* First boot: a new EC P-256 key, from the DRBG seeded by the hardware entropy source. */
    if (mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0 ||
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), identity_random, NULL) != 0) {
        return false;
    }
    int n = mbedtls_pk_write_key_der(&key, buf, sizeof buf); /* written at the end of buf */
#if HELLO_KEY_LADDER
    /* Wrapped, then written over the key sector: a plain key left there by a plain-key build is gone too. */
    static uint8_t wrapped[320];
    size_t wrapped_len = 0;
    bool ok = n > 0 && ladder_wrap(kek, buf + sizeof buf - n, (size_t)n, wrapped, sizeof wrapped, &wrapped_len) == 0 &&
              record_write(KEY_OFFSET, MAGIC_WRAPPED, wrapped, wrapped_len);
#else
    bool ok = n > 0 && record_write(KEY_OFFSET, MAGIC_KEY, buf + sizeof buf - n, (size_t)n);
#endif
    mbedtls_platform_zeroize(buf, sizeof buf);
    return ok;
}

const char *identity_thing_name(void)
{
    return thing_name;
}

mbedtls_pk_context *identity_key(void)
{
    return &key;
}

int identity_csr_pem(unsigned char *buf, size_t len)
{
    char subject[THING_NAME_MAX + 4];
    snprintf(subject, sizeof subject, "CN=%s", thing_name);
    mbedtls_x509write_csr csr;
    mbedtls_x509write_csr_init(&csr);
    mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
    mbedtls_x509write_csr_set_key(&csr, &key);
    int rc = mbedtls_x509write_csr_set_subject_name(&csr, subject);
    if (rc == 0) {
        rc = mbedtls_x509write_csr_pem(&csr, buf, len, identity_random, NULL);
    }
    mbedtls_x509write_csr_free(&csr);
    return rc;
}

bool identity_cert_matches(const char *cert_pem)
{
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    bool ok = mbedtls_x509_crt_parse(&crt, (const unsigned char *)cert_pem, strlen(cert_pem) + 1) == 0 &&
              mbedtls_pk_check_pair(&crt.pk, &key, identity_random, NULL) == 0;
    mbedtls_x509_crt_free(&crt);
    return ok;
}

const identity_config_t *identity_config(void)
{
    size_t len;
    const uint8_t *data = record_read(CONFIG_OFFSET, MAGIC_CONFIG, &len);
    return data && len == sizeof(identity_config_t) ? (const identity_config_t *)data : NULL;
}

bool identity_config_save(const identity_config_t *config)
{
    return record_write(CONFIG_OFFSET, MAGIC_CONFIG, config, sizeof *config);
}

const wifi_config_t *identity_wifi(void)
{
    return have_wifi ? &wifi : NULL;
}

bool identity_wifi_save(const wifi_config_t *w)
{
#if HELLO_KEY_LADDER
    static uint8_t wrapped[256];
    size_t wrapped_len = 0;
    bool ok = ladder_wrap(kek, (const uint8_t *)w, sizeof *w, wrapped, sizeof wrapped, &wrapped_len) == 0 &&
              record_write(WIFI_OFFSET, MAGIC_WIFI_WRAPPED, wrapped, wrapped_len);
#else
    bool ok = record_write(WIFI_OFFSET, MAGIC_WIFI, w, sizeof *w);
#endif
    if (ok && w != &wifi) {
        wifi = *w;
    }
    have_wifi = have_wifi || ok;
    return ok;
}

bool identity_reset(bool forget_all)
{
    bool ok = record_write(CONFIG_OFFSET, 0, NULL, 0);
    if (forget_all) {
        ok = record_write(KEY_OFFSET, 0, NULL, 0) && ok;
        ok = record_write(WIFI_OFFSET, 0, NULL, 0) && ok;
    }
    return ok;
}
