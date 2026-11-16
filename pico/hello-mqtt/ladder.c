#include "ladder.h"

#include <stdbool.h>
#include <string.h>

#include "hardware/platform_defs.h"
#include "hardware/regs/addressmap.h"
#include "hardware/structs/otp.h"
#include "identity.h"
#include "mbedtls/entropy.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"
#include "pico/bootrom.h"
#include "pico/platform.h"

/* trng_rp2350.c; mbedTLS 3 no longer declares it in a public header. */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen);

#ifndef OTP_ROOT_PAGE
#define OTP_ROOT_PAGE 60
#endif
/* User OTP is pages 3 to 60, and Raspberry Pi Connect uses rows in page 3. The soft lock covers a whole page,
 * so the root has a page to itself. */
#if OTP_ROOT_PAGE < 4 || OTP_ROOT_PAGE > 60
#error "OTP_ROOT_PAGE must be 4-60"
#endif

#define ROOT_ROW (OTP_ROOT_PAGE * NUM_OTP_PAGE_ROWS) /* page 60: row 0xF00 */
#define ROOT_ROWS 16u                                /* 32 bytes, 16 bits per ECC row */
#define MARKER_ROW (ROOT_ROW + ROOT_ROWS)            /* page 60: row 0xF10 */
#define MARKER 0x4c31u                               /* "L1": written after the root, so a root without it was cut short */

static const uint8_t MAGIC[4] = {'R', 'P', 'K', 'W'};
#define FORMAT 1u
#define NONCE_LEN 12u
#define TAG_LEN 16u

/* OTP aliases: ECC reads give 16 bits per row; raw reads give the 24-bit row; neither faults. A guarded read
 * faults on a bad row, which is what we want once the marker says the root is complete. */
static uint16_t ecc_row(uint32_t row)
{
    return *(const volatile uint16_t *)(OTP_DATA_BASE + row * 2u);
}

static uint32_t raw_row(uint32_t row)
{
    return *(const volatile uint32_t *)(OTP_DATA_RAW_BASE + row * 4u);
}

static bool all_zero(const uint8_t *p, size_t n)
{
    uint8_t acc = 0;
    while (n--) {
        acc |= *p++;
    }
    return acc == 0;
}

ladder_state_t ladder_state(void)
{
    if (ecc_row(MARKER_ROW) == MARKER) {
        return LADDER_PROGRAMMED;
    }
    uint32_t acc = 0;
    for (uint32_t row = ROOT_ROW; row <= MARKER_ROW; row++) {
        acc |= raw_row(row);
    }
    return acc == 0 ? LADDER_BLANK : LADDER_INTERRUPTED;
}

static int otp_write(const void *data, uint32_t len, uint32_t row)
{
    otp_cmd_t cmd = {.flags = row | OTP_CMD_ECC_BITS | OTP_CMD_WRITE_BITS};
    return rom_func_otp_access((uint8_t *)data, len, cmd) == BOOTROM_OK ? 0 : -1;
}

int ladder_write_root(void)
{
    if (ladder_state() != LADDER_BLANK) {
        return -1;
    }
    uint8_t root[32] __attribute__((aligned(4)));
    size_t got = 0;
    /* Straight from the TRNG (mbedtls_hardware_poll is trng_rp2350.c), not through the DRBG. */
    if (mbedtls_hardware_poll(NULL, root, sizeof root, &got) != 0 || got != sizeof root || all_zero(root, sizeof root)) {
        mbedtls_platform_zeroize(root, sizeof root);
        return -2;
    }
    int rc = otp_write(root, sizeof root, ROOT_ROW);
    mbedtls_platform_zeroize(root, sizeof root);
    if (rc == 0) {
        uint16_t marker __attribute__((aligned(4))) = MARKER;
        rc = otp_write(&marker, sizeof marker, MARKER_ROW);
    }
    return rc == 0 && ladder_state() == LADDER_PROGRAMMED ? 0 : -3;
}

int ladder_kek(uint8_t kek[32])
{
    static bool done;
    if (done || ladder_state() != LADDER_PROGRAMMED) {
        return -1;
    }
    done = true;
    uint8_t root[32];
    for (uint32_t i = 0; i < ROOT_ROWS / 2u; i++) { /* each 32-bit guarded read is two ECC rows */
        uint32_t w = *(const volatile uint32_t *)(OTP_DATA_GUARDED_BASE + (ROOT_ROW + 2u * i) * 2u);
        memcpy(root + 4u * i, &w, sizeof w);
    }
    int rc = all_zero(root, sizeof root) ? -2 : 0;
    if (rc == 0) {
        const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        rc = mbedtls_md_hmac(md, root, sizeof root, (const unsigned char *)LADDER_LABEL, strlen(LADDER_LABEL), kek);
    }
    mbedtls_platform_zeroize(root, sizeof root);
    /* Secure and Non-secure reads of the page are refused until the next reset. */
    otp_hw->sw_lock[OTP_ROOT_PAGE] = OTP_SW_LOCK0_BITS;
    return rc;
}

static size_t header(uint8_t *out)
{
    size_t n = strlen(LADDER_LABEL);
    memcpy(out, MAGIC, sizeof MAGIC);
    out[4] = FORMAT;
    out[5] = (uint8_t)n;
    memcpy(out + 6, LADDER_LABEL, n);
    return 6 + n;
}

int ladder_wrap(const uint8_t kek[32], const uint8_t *in, size_t len, uint8_t *out, size_t cap, size_t *out_len)
{
    size_t h = 6 + strlen(LADDER_LABEL);
    if (cap < h + NONCE_LEN + len + TAG_LEN) {
        return -1;
    }
    header(out);
    uint8_t *nonce = out + h, *body = nonce + NONCE_LEN;
    if (identity_random(NULL, nonce, NONCE_LEN) != 0) {
        return -1;
    }
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, kek, 256) ||
             mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, len, nonce, NONCE_LEN, out, h, in, body, TAG_LEN,
                                       body + len);
    mbedtls_gcm_free(&gcm);
    *out_len = h + NONCE_LEN + len + TAG_LEN;
    return rc ? -1 : 0;
}

int ladder_unwrap(const uint8_t kek[32], const uint8_t *blob, size_t len, uint8_t *out, size_t cap, size_t *out_len)
{
    uint8_t expect[64];
    size_t h = header(expect);
    if (len < h + NONCE_LEN + TAG_LEN || memcmp(blob, expect, h) != 0 || cap < len - h - NONCE_LEN - TAG_LEN) {
        return -1;
    }
    size_t body_len = len - h - NONCE_LEN - TAG_LEN;
    const uint8_t *nonce = blob + h, *body = nonce + NONCE_LEN, *tag = body + body_len;
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, kek, 256) ||
             mbedtls_gcm_auth_decrypt(&gcm, body_len, nonce, NONCE_LEN, blob, h, tag, TAG_LEN, body, out);
    mbedtls_gcm_free(&gcm);
    *out_len = body_len;
    return rc ? -2 : 0; /* -2: another chip's key, or damaged */
}

unsigned ladder_page(void)
{
    return OTP_ROOT_PAGE;
}

unsigned ladder_first_row(void)
{
    return ROOT_ROW;
}

unsigned ladder_stepping(void)
{
    return rp2350_rom_version(); /* 2, 3, 4 for A2, A3, A4 */
}
