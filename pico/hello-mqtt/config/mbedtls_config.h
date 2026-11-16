/* mbedTLS for hello-mqtt (the mbedTLS 3.x in Pico SDK 2.x): a TLS 1.2 client with ECDHE, an EC P-256 key
 * made on the Pico, a PKCS#10 CSR, and X.509 parsing. AWS IoT's endpoints chain to Amazon Root CA 1 (RSA). */
#ifndef HELLO_MBEDTLS_CONFIG_H
#define HELLO_MBEDTLS_CONFIG_H

#define MBEDTLS_PLATFORM_C
#define MBEDTLS_NO_PLATFORM_ENTROPY
/* Entropy from mbedtls_hardware_poll(): the RP2350 TRNG (trng_rp2350.c), or pico_rand on RP2040. */
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
/* The clock comes from SNTP (main.c). Until it is set, certificate dates fail closed. */
#define MBEDTLS_PLATFORM_TIME_ALT
#define MBEDTLS_PLATFORM_MS_TIME_ALT
/* The SDK's altcp_tls_mbedtls reads private fields of the SSL context. */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS

#define MBEDTLS_AES_C
#define MBEDTLS_AES_FEWER_TABLES
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECDSA_DETERMINISTIC
#define MBEDTLS_HMAC_DRBG_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21

#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_OID_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_PEM_WRITE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_WRITE_C

#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_CREATE_C
#define MBEDTLS_X509_CSR_WRITE_C

#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
/* AWS IoT does not negotiate a smaller record size, so incoming records can be 16 KB. */
#define MBEDTLS_SSL_IN_CONTENT_LEN 16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096

#endif
