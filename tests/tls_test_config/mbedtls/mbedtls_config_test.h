/**
 * Test-only mbedTLS configuration for the BerryAPI host tests.
 *
 * The shipped library compiles mbedTLS in client-only mode (see the vendored
 * mbedtls_config.h). The host tests need the *server* role as well, so that a
 * TLS handshake can be exercised end to end against a real peer instead of a
 * fake. This config layers the server role on top of the vendored one; it is
 * used only for the objects under build/mbedtls-test/ and is never part of the
 * injectable library.
 *
 * The whole mbedTLS tree must be compiled with ONE config, because the server
 * handshake code calls helpers that live in ssl_tls.c/ssl_msg.c behind the
 * MBEDTLS_SSL_SRV_C guard. Mixing a client-only ssl_tls.o with server TUs
 * would not link, so the test build recompiles every mbedTLS source with this
 * config rather than sharing the shipping objects.
 */
#ifndef BERRY_MBEDTLS_TEST_CONFIG_H
#define BERRY_MBEDTLS_TEST_CONFIG_H

#include "mbedtls/mbedtls_config.h"

/* Server role, for the in-test TLS server. */
#define MBEDTLS_SSL_SRV_C

/* Debug traces, so a failing test handshake explains itself on stderr. */
#define MBEDTLS_DEBUG_C

/* TLS 1.3 session tickets need MBEDTLS_SSL_TICKET_C, which the vendored
 * (client) config leaves off. Drop the feature for the test build so
 * check_config.h is satisfied without vendoring the ticket module. */
#undef MBEDTLS_SSL_SESSION_TICKETS

#endif  /* BERRY_MBEDTLS_TEST_CONFIG_H */
