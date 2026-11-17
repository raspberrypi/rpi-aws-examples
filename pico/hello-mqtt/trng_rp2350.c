/*
 * mbedTLS entropy from the RP2350's TRNG, with the TRNG's health tests on.
 *
 * The SDK's pico_rand reads the same TRNG with its health tests bypassed and mixes the samples into a fast,
 * non-cryptographic generator: fine for jitter, not for keys. This driver follows the TRNG's documented
 * sequence instead (RP2350 datasheet, section "TRNG"): health tests and decorrelators on, a failed block
 * discarded and generated again, and a reset after an autocorrelation failure, which stops the block.
 *
 * The datasheet suggests a sample count of 20-25. On a Pico 2 W that fails the autocorrelation test most
 * of the time; 250 and above passed every block in testing, so 1000 is used, about 6 ms per 24 bytes.
 */
#include <string.h>

#include "hardware/structs/trng.h"
#include "mbedtls/entropy.h"
#include "pico/time.h"

#if PICO_RAND_ENTROPY_SRC_TRNG
#error "build pico_rand with PICO_RAND_ENTROPY_SRC_TRNG=0: it would bypass the health tests this driver relies on"
#endif

#define SAMPLE_COUNT 1000u
#define ROSC_CHAIN_LENGTH 0u
#define BLOCK_TIMEOUT_MS 100u

static void trng_reset(void)
{
    trng_hw->trng_sw_reset = 1;
    /* The datasheet's boot ROM sequence reads the register back twice as the delay a reset needs. */
    (void)trng_hw->trng_sw_reset;
    (void)trng_hw->trng_sw_reset;
}

static void trng_start(void)
{
    trng_hw->trng_debug_control = 0; /* health tests and decorrelators on */
    trng_hw->trng_config = ROSC_CHAIN_LENGTH;
    trng_hw->sample_cnt1 = SAMPLE_COUNT;
    trng_hw->rnd_source_enable = TRNG_RND_SOURCE_ENABLE_RND_SRC_EN_BITS;
}

/* One 192-bit block, or false after BLOCK_TIMEOUT_MS. */
static bool trng_block(uint8_t out[24])
{
    bool ok = false;
    trng_start();
    absolute_time_t deadline = make_timeout_time_ms(BLOCK_TIMEOUT_MS);
    while (!time_reached(deadline)) {
        uint32_t isr = trng_hw->rng_isr;
        if (isr & TRNG_RNG_ISR_EHR_VALID_BITS) {
            uint32_t words[6];
            for (int i = 0; i < 6; i++) {
                words[i] = trng_hw->ehr_data[i]; /* reading the last word starts the next block */
            }
            memcpy(out, words, sizeof words);
            trng_hw->rng_icr = TRNG_RNG_ISR_EHR_VALID_BITS;
            ok = true;
            break;
        }
        if (isr & TRNG_RNG_ISR_AUTOCORR_ERR_BITS) {
            trng_reset(); /* the TRNG stops after this error until it is reset */
            trng_start();
        } else if (isr & (TRNG_RNG_ISR_VN_ERR_BITS | TRNG_RNG_ISR_CRNGT_ERR_BITS)) {
            trng_hw->rng_icr = isr; /* that block is discarded; the next one starts by itself */
        }
    }
    trng_hw->rnd_source_enable = 0;
    return ok;
}

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    (void)data;
    size_t done = 0;
    while (done < len) {
        uint8_t block[24];
        if (!trng_block(block)) {
            *olen = done;
            return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
        }
        size_t n = len - done < sizeof block ? len - done : sizeof block;
        memcpy(output + done, block, n);
        done += n;
    }
    *olen = len;
    return 0;
}
