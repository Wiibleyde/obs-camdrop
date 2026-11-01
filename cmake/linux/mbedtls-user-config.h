/* mbedTLS options for the Linux build of Camdrop: same TLS behaviour as the obs-deps build used on
 * Windows and macOS (TLS 1.2), plus pthread locking since every HTTPS connection has its own thread. */
#undef MBEDTLS_SSL_PROTO_TLS1_3
#undef MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_ENABLED
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL_ENABLED
#undef MBEDTLS_SSL_EARLY_DATA
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD
