/*
 * The key ladder on the Pico 2 W (built with -DHELLO_KEY_LADDER=ON):
 *
 *   root key, 256 bits from the TRNG, in OTP page OTP_ROOT_PAGE (default 60), written once by OTP-INIT
 *     └─ KEK = HMAC-SHA256(root, "AWS_cert_keys:v1")      at boot; then the page is locked until reset
 *          └─ AES-256-GCM wrap of the device key           in flash
 *               └─ the plain key                           in RAM only
 *
 * A copy of the flash is useless on any other Pico. The firmware never writes OTP by itself: only the
 * OTP-INIT command does, which pico/tools/pico-otp-init.py sends after its dry run and your confirmation.
 */
#ifndef LADDER_H
#define LADDER_H

#include <stddef.h>
#include <stdint.h>

#define LADDER_LABEL "AWS_cert_keys:v1"

typedef enum { LADDER_BLANK, LADDER_PROGRAMMED, LADDER_INTERRUPTED } ladder_state_t;

/* The state of the root's rows. Reads that cannot fault; valid until ladder_kek() locks the page. */
ladder_state_t ladder_state(void);

/* Writes a new root from the TRNG, then the marker row. PERMANENT. Only on a blank page. Returns 0 or <0. */
int ladder_write_root(void);

/* Derives the KEK, wipes the root from RAM and locks the page until reset. Once per boot. Returns 0 or <0. */
int ladder_kek(uint8_t kek[32]);

/* "RPKW" | format 1 | len(label) | label | 12-byte nonce | AES-256-GCM(in) | 16-byte tag; the header is the
 * additional authenticated data. The same format as the Raspberry Pi computers' key helper. */
int ladder_wrap(const uint8_t kek[32], const uint8_t *in, size_t len, uint8_t *out, size_t cap, size_t *out_len);
int ladder_unwrap(const uint8_t kek[32], const uint8_t *blob, size_t len, uint8_t *out, size_t cap, size_t *out_len);

/* For STATUS: the page, its first row, and the chip's stepping (2 = A2, 4 = A4). */
unsigned ladder_page(void);
unsigned ladder_first_row(void);
unsigned ladder_stepping(void);

#endif
