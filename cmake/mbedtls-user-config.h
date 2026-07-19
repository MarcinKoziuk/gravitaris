// Appended to MbedTLS's default mbedtls_config.h (via MBEDTLS_USER_CONFIG_FILE)
// for libdatachannel: its DTLS transport unconditionally references the
// SRTP keying-material extension (RFC 5764) even with libdatachannel's own
// NO_MEDIA=1, and MbedTLS ships it disabled by default.
#define MBEDTLS_SSL_DTLS_SRTP
