/*****************************************************************************
* Copyright (c) 2016, Cisco Systems, Inc.
* All rights reserved.

* Redistribution and use in source and binary forms, with or without modification,
* are permitted provided that the following conditions are met:
*
* 1. Redistributions of source code must retain the above copyright notice,
*    this list of conditions and the following disclaimer.
*
* 2. Redistributions in binary form must reproduce the above copyright notice,
*    this list of conditions and the following disclaimer in the documentation
*    and/or other materials provided with the distribution.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
* DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
* SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
* CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
* USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*****************************************************************************/
/*
 * This module is not part of libacvp.  Rather, it's a simple app that
 * demonstrates how to use libacvp. Software that use libacvp
 * will need to implement a similar module.
 *
 * It will default to 127.0.0.1 port 443 if no arguments are given.
 */
//#define OPENSSL_FIPSAPI

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include "acvp.h"
#ifdef USE_MURL
#include <murl/murl.h>
#else
#include <curl/curl.h>
#endif
#include <openssl/evp.h>
#include <openssl/aes.h>
#include <openssl/bio.h>
#include <openssl/obj_mac.h>
#include <openssl/err.h>
#include <openssl/hmac.h>
#include <openssl/cmac.h>
#include <openssl/rsa.h>
#include <openssl/bn.h>
#include <openssl/rsa.h>
#include <openssl/fips.h>
#include <openssl/fipssyms.h>

#ifdef ACVP_NO_RUNTIME
#include "app_lcl.h"
#include <openssl/fips_rand.h>
#include <openssl/fips.h>
extern int fips_selftest_fail;
extern int fips_mode;
#endif

static ACVP_RESULT app_aes_handler_aead(ACVP_TEST_CASE *test_case);
static ACVP_RESULT app_aes_keywrap_handler(ACVP_TEST_CASE *test_case);
static ACVP_RESULT app_aes_handler(ACVP_TEST_CASE *test_case);
static ACVP_RESULT app_des_handler(ACVP_TEST_CASE *test_case);
static ACVP_RESULT app_sha_handler(ACVP_TEST_CASE *test_case);
static ACVP_RESULT app_hmac_handler(ACVP_TEST_CASE *test_case);
static ACVP_RESULT app_cmac_handler(ACVP_TEST_CASE *test_case);
static ACVP_RESULT app_rsa_handler(ACVP_TEST_CASE *test_case);
static ACVP_RESULT app_rsa_siggen_handler(ACVP_TEST_CASE *test_case);
#ifdef ACVP_NO_RUNTIME
static ACVP_RESULT app_drbg_handler(ACVP_TEST_CASE *test_case);
#endif


#define DEFAULT_SERVER "127.0.0.1"
#define DEFAULT_PORT 443
#define DEFAULT_CA_CHAIN "certs/acvp-private-root-ca.crt.pem"
#define DEFAULT_CERT "certs/sto-labsrv2-client-cert.pem"
#define DEFAULT_KEY "certs/sto-labsrv2-client-key.pem"


char *server;
int port;
char *ca_chain_file;
char *cert_file;
char *key_file;
char *path_segment;
static EVP_CIPHER_CTX cipher_ctx;  /* need to maintain across calls for MCT */

#define CHECK_ENABLE_CAP_RV(rv) \
    if (rv != ACVP_SUCCESS) { \
        printf("Failed to register AES GCM capability with libacvp (rv=%d)\n", rv); \
        exit(1); \
    }


/*
 * Read the operational parameters from the various environment
 * variables.
 */
static void setup_session_parameters()
{
    char *tmp;

    server = getenv("ACV_SERVER");
    if (!server) server = DEFAULT_SERVER;

    tmp = getenv("ACV_PORT");
    if (tmp) port = atoi(tmp);
    if (!port) port = DEFAULT_PORT;

    path_segment = getenv("ACV_URI_PREFIX");
    if (!path_segment) path_segment = "";

    ca_chain_file = getenv("ACV_CA_FILE");
    if (!ca_chain_file) ca_chain_file = DEFAULT_CA_CHAIN;

    cert_file = getenv("ACV_CERT_FILE");
    if (!cert_file) cert_file = DEFAULT_CERT;

    key_file = getenv("ACV_KEY_FILE");
    if (!key_file) key_file = DEFAULT_KEY;

    printf("Using the following parameters:\n\n");
    printf("    ACV_SERVER:     %s\n", server);
    printf("    ACV_PORT:       %d\n", port);
    printf("    ACV_URI_PREFIX: %s\n", path_segment);
    printf("    ACV_CA_FILE:    %s\n", ca_chain_file);
    printf("    ACV_CERT_FILE:  %s\n", cert_file);
    printf("    ACV_KEY_FILE:   %s\n\n", key_file);
}

/*
 * This is a minimal and rudimentary logging handler.
 * libacvp calls this function to for debugs, warnings,
 * and errors.
 */
ACVP_RESULT progress(char *msg)
{
    printf("%s", msg);
    return ACVP_SUCCESS;
}

static void print_usage(void)
{
    printf("\nInvalid usage...\n");
    printf("acvp_app does not require any argument, however logging level can be\n");
    printf("controlled using:\n");
    printf("      -none\n");
    printf("      -error\n");
    printf("      -warn\n");
    printf("      -status(default)\n");
    printf("      -info\n");
    printf("      -verbose\n");
    printf("\n");
    printf("In addition some options are passed to acvp_app using\n");
    printf("environment variables.  The following variables can be set:\n\n");
    printf("    ACV_SERVER (when not set, defaults to %s)\n", DEFAULT_SERVER);
    printf("    ACV_PORT (when not set, defaults to %d)\n", DEFAULT_PORT);
    printf("    ACV_URI_PREFIX (when not set, defaults to null)\n");
    printf("    ACV_CA_FILE (when not set, defaults to %s)\n", DEFAULT_CA_CHAIN);
    printf("    ACV_CERT_FILE (when not set, defaults to %s)\n", DEFAULT_CERT);
    printf("    ACV_KEY_FILE (when not set, defaults to %s)\n\n", DEFAULT_KEY);
    printf("The CA certificates, cert and key should be PEM encoded. There should be no\n");
    printf("password on the key file.\n");
}

int main(int argc, char **argv)
{
    ACVP_RESULT rv;
    ACVP_CTX *ctx;
    char ssl_version[10];
    ACVP_LOG_LVL level = ACVP_LOG_LVL_STATUS;
    char value[] = "same";

    if (argc > 2) {
        print_usage();
        return 1;
    }

    argv++;
    argc--;
    while (argc >= 1) {
        if (strcmp(*argv, "-info") == 0) {
            level = ACVP_LOG_LVL_INFO;
        }
        if (strcmp(*argv, "-status") == 0) {
            level = ACVP_LOG_LVL_STATUS;
        }
        if (strcmp(*argv, "-warn") == 0) {
            level = ACVP_LOG_LVL_WARN;
        }
        if (strcmp(*argv, "-error") == 0) {
            level = ACVP_LOG_LVL_ERR;
        }
        if (strcmp(*argv, "-none") == 0) {
            level = ACVP_LOG_LVL_NONE;
        }
        if (strcmp(*argv, "-verbose") == 0) {
            level = ACVP_LOG_LVL_VERBOSE;
        }
        if (strcmp(*argv, "-help") == 0) {
            print_usage();
            return 1;
        }
    argv++;
    argc--;
    }

#ifdef ACVP_NO_RUNTIME
    fips_selftest_fail = 0;
    fips_mode = 0;
    fips_algtest_init_nofips();
#endif

    EVP_CIPHER_CTX_cleanup(&cipher_ctx);
    setup_session_parameters();

    /*
     * We begin the libacvp usage flow here.
     * First, we create a test session context.
     */
    rv = acvp_create_test_session(&ctx, &progress, level);
    if (rv != ACVP_SUCCESS) {
        printf("Failed to create ACVP context\n");
        exit(1);
    }

    /*
     * Next we specify the ACVP server address
     */
    rv = acvp_set_server(ctx, server, port);
    if (rv != ACVP_SUCCESS) {
        printf("Failed to set server/port\n");
        exit(1);
    }

    /*
     * Setup the vendor attributes
     */
    rv = acvp_set_vendor_info(ctx, "Cisco Systems", "www.cisco.com", "Barry Fussell", "bfussell@cisco.com");
    if (rv != ACVP_SUCCESS) {
        printf("Failed to set vendor info\n");
        exit(1);
    }

    /*
     * Setup the crypto module attributes
     */
    snprintf(ssl_version, 10, "%08x", (unsigned int)SSLeay());
    rv = acvp_set_module_info(ctx, "OpenSSL", "software", ssl_version, "FOM 6.2a");
    if (rv != ACVP_SUCCESS) {
        printf("Failed to set module info\n");
        exit(1);
    }

    /*
     * Set the path segment prefix if needed
     */
     if (strnlen(path_segment, 255) > 0) {
        rv = acvp_set_path_segment(ctx, path_segment);
        if (rv != ACVP_SUCCESS) {
            printf("Failed to set URI prefix\n");
            exit(1);
        }
     }

    /*
     * Next we provide the CA certs to be used by libacvp
     * to verify the ACVP TLS certificate.
     */
    rv = acvp_set_cacerts(ctx, ca_chain_file);
    if (rv != ACVP_SUCCESS) {
        printf("Failed to set CA certs\n");
        exit(1);
    }

    /*
     * Specify the certificate and private key the client should used
     * for TLS client auth.
     */
    rv = acvp_set_certkey(ctx, cert_file, key_file);
    if (rv != ACVP_SUCCESS) {
        printf("Failed to set TLS cert/key\n");
        exit(1);
    }

    /*
	 * Enable RSA keygen...
	 */
	rv = acvp_enable_rsa_cap(ctx, ACVP_RSA, &app_rsa_siggen_handler);
	CHECK_ENABLE_CAP_RV(rv);
	rv = acvp_enable_prereq_cap(ctx, ACVP_RSA, ACVP_PREREQ_DRBG, value);
	CHECK_ENABLE_CAP_RV(rv);
	rv = acvp_enable_prereq_cap(ctx, ACVP_RSA, ACVP_PREREQ_SHA, value);
	CHECK_ENABLE_CAP_RV(rv);

//	rv = acvp_enable_rsa_cap_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_KEYGEN, ACVP_PUB_EXP, RSA_PUB_EXP_FIXED);
//	CHECK_ENABLE_CAP_RV(rv);
//	rv = acvp_enable_rsa_cap_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_KEYGEN, ACVP_RSA_INFO_GEN_BY_SERVER, 0);
//	CHECK_ENABLE_CAP_RV(rv);
//
//    BIGNUM *expo = BN_new();
//	unsigned long mm = RSA_F4;
//	if (!BN_set_word(expo, mm)) {
//		printf("Bignum API fail\n");
//		return ACVP_CRYPTO_MODULE_FAIL;
//	}
//	rv = acvp_enable_rsa_bignum_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_KEYGEN, ACVP_FIXED_PUB_EXP_VAL, expo);
//	CHECK_ENABLE_CAP_RV(rv);
//	 //ENABLES RAND PQ VALUE 1 or 3 -- "provPC" or "provRP"
//	 //not to be used in conjunction with other randPQ values
//	 rv = acvp_enable_rsa_cap_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_KEYGEN, ACVP_RAND_PQ, RSA_RAND_PQ_B34);
//	 CHECK_ENABLE_CAP_RV(rv);
//	 rv = acvp_enable_rsa_primes_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_KEYGEN, ACVP_CAPS_PROV_PRIME, MOD_RSA_2048, ACVP_RSA_SHA_1);
//	 CHECK_ENABLE_CAP_RV(rv);
//	 rv = acvp_enable_rsa_primes_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_KEYGEN, ACVP_CAPS_PROV_PRIME, MOD_RSA_2048, ACVP_RSA_SHA_224);
//	 CHECK_ENABLE_CAP_RV(rv);
//	 rv = acvp_enable_rsa_primes_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_KEYGEN, ACVP_CAPS_PROV_PRIME, MOD_RSA_2048, ACVP_RSA_SHA_512);
//	 CHECK_ENABLE_CAP_RV(rv);
//	 rv = acvp_enable_rsa_primes_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_KEYGEN, ACVP_CAPS_PROV_PRIME, MOD_RSA_4096, ACVP_RSA_SHA_1);
//	 CHECK_ENABLE_CAP_RV(rv);
//	 rv = acvp_enable_rsa_primes_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_KEYGEN, ACVP_CAPS_PROV_PRIME, MOD_RSA_4096, ACVP_RSA_SHA_224);
//	 CHECK_ENABLE_CAP_RV(rv);

	// ENABLES RAND PQ VALUE 2 or 5 -- "probPC" or "probRP"
	// not to be used in conjunction with other randPQ values
//	 rv = acvp_enable_rsa_cap_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_KEYGEN, ACVP_RAND_PQ, RSA_RAND_PQ_B33);
//	 CHECK_ENABLE_CAP_RV(rv);
//	 rv = acvp_enable_rsa_primes_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_KEYGEN, ACVP_CAPS_PROB_PRIME, MOD_PRIME_2048, PRIME_TEST_TBLC2_NAME);
//	 CHECK_ENABLE_CAP_RV(rv);
//	 rv = acvp_enable_rsa_primes_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_KEYGEN, ACVP_CAPS_PROB_PRIME, MOD_PRIME_4096, PRIME_TEST_TBLC3_NAME);
//	 CHECK_ENABLE_CAP_RV(rv);
//	 rv = acvp_enable_rsa_primes_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_KEYGEN, ACVP_CAPS_PROB_PRIME, MOD_PRIME_4096, PRIME_TEST_TBLC2_NAME);
//	 CHECK_ENABLE_CAP_RV(rv);

	// ENABLES RAND PQ VALUE 4 -- "bothPC"
	// not to be used in conjunction with other randPQ values
//	rv = acvp_enable_rsa_cap_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_KEYGEN, ACVP_RAND_PQ, RSA_RAND_PQ_B35);
//	CHECK_ENABLE_CAP_RV(rv);
//	rv = acvp_enable_rsa_primes_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_KEYGEN, ACVP_CAPS_PROV_PROB_PRIME, MOD_PRIME_2048, PRIME_TEST_TBLC2_NAME);
//	CHECK_ENABLE_CAP_RV(rv);
//	rv = acvp_enable_rsa_primes_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_KEYGEN, ACVP_CAPS_PROV_PROB_PRIME, MOD_PRIME_2048, ACVP_RSA_SHA_224);
//	CHECK_ENABLE_CAP_RV(rv);
//	rv = acvp_enable_rsa_primes_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_KEYGEN, ACVP_CAPS_PROV_PROB_PRIME, MOD_PRIME_2048, ACVP_RSA_SHA_512);
//	CHECK_ENABLE_CAP_RV(rv);
//	rv = acvp_enable_rsa_primes_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_KEYGEN, ACVP_CAPS_PROV_PROB_PRIME, MOD_PRIME_3072, PRIME_TEST_TBLC2_NAME);
//	CHECK_ENABLE_CAP_RV(rv);
//	rv = acvp_enable_rsa_primes_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_KEYGEN, ACVP_CAPS_PROV_PROB_PRIME, MOD_PRIME_3072, ACVP_RSA_PRIME_SHA_1);
//	CHECK_ENABLE_CAP_RV(rv);
//	rv = acvp_enable_rsa_primes_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_KEYGEN, ACVP_CAPS_PROV_PROB_PRIME, MOD_PRIME_4096, ACVP_RSA_SHA_224);
//	CHECK_ENABLE_CAP_RV(rv);
//	rv = acvp_enable_rsa_primes_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_KEYGEN, ACVP_CAPS_PROV_PROB_PRIME, MOD_PRIME_4096, PRIME_TEST_TBLC3_NAME);
//	CHECK_ENABLE_CAP_RV(rv);
//	rv = acvp_enable_rsa_primes_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_KEYGEN, ACVP_CAPS_PROV_PROB_PRIME, MOD_PRIME_4096, PRIME_TEST_TBLC2_NAME);
//	CHECK_ENABLE_CAP_RV(rv);

	// BN_free(expo);


/*// RSA w/ sigType: X9.31
    rv = acvp_enable_rsa_cap_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, ACVP_SIG_TYPE, RSA_SIG_TYPE_X931);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, RSA_SIG_TYPE_X931, MOD_RSA_2048, ACVP_RSA_SHA_224);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, RSA_SIG_TYPE_X931, MOD_RSA_2048, ACVP_RSA_SHA_256);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, RSA_SIG_TYPE_X931, MOD_RSA_2048, ACVP_RSA_SHA_512);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, RSA_SIG_TYPE_X931, MOD_RSA_3072, ACVP_RSA_SHA_224);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, RSA_SIG_TYPE_X931, MOD_RSA_3072, ACVP_RSA_SHA_256);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, RSA_SIG_TYPE_X931, MOD_RSA_3072, ACVP_RSA_SHA_512);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, RSA_SIG_TYPE_X931, MOD_RSA_4096, ACVP_RSA_SHA_224);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, RSA_SIG_TYPE_X931, MOD_RSA_4096, ACVP_RSA_SHA_256);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, RSA_SIG_TYPE_X931, MOD_RSA_4096, ACVP_RSA_SHA_512);
    CHECK_ENABLE_CAP_RV(rv);

// RSA w/ sigType: PKCS1v1.5
	rv = acvp_enable_rsa_cap_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, ACVP_SIG_TYPE, RSA_SIG_TYPE_PKCS1V15);
	CHECK_ENABLE_CAP_RV(rv);
	rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, RSA_SIG_TYPE_PKCS1V15, MOD_RSA_2048, ACVP_RSA_SHA_224);
	CHECK_ENABLE_CAP_RV(rv);
	rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, RSA_SIG_TYPE_PKCS1V15, MOD_RSA_2048, ACVP_RSA_SHA_256);
	CHECK_ENABLE_CAP_RV(rv);
	rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, RSA_SIG_TYPE_PKCS1V15, MOD_RSA_2048, ACVP_RSA_SHA_512);
	CHECK_ENABLE_CAP_RV(rv);
	rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, RSA_SIG_TYPE_PKCS1V15, MOD_RSA_3072, ACVP_RSA_SHA_224);
	CHECK_ENABLE_CAP_RV(rv);
	rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, RSA_SIG_TYPE_PKCS1V15, MOD_RSA_3072, ACVP_RSA_SHA_256);
	CHECK_ENABLE_CAP_RV(rv);
	rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, RSA_SIG_TYPE_PKCS1V15, MOD_RSA_3072, ACVP_RSA_SHA_512);
	CHECK_ENABLE_CAP_RV(rv);
	rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, RSA_SIG_TYPE_PKCS1V15, MOD_RSA_4096, ACVP_RSA_SHA_224);
	CHECK_ENABLE_CAP_RV(rv);
	rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, RSA_SIG_TYPE_PKCS1V15, MOD_RSA_4096, ACVP_RSA_SHA_256);
	CHECK_ENABLE_CAP_RV(rv);
	rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, RSA_SIG_TYPE_PKCS1V15, MOD_RSA_4096, ACVP_RSA_SHA_512);
	CHECK_ENABLE_CAP_RV(rv);

// RSA w/ sigType: PKCS1PSS -- has saltSigGen
	rv = acvp_enable_rsa_cap_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, ACVP_SIG_TYPE, RSA_SIG_TYPE_PKCS1PSS);
	CHECK_ENABLE_CAP_RV(rv);
	rv = acvp_enable_rsa_cap_sig_type_salt_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, RSA_SIG_TYPE_PKCS1PSS, MOD_RSA_2048, ACVP_RSA_SHA_224, RSA_SALT_SIGGEN_28);
	CHECK_ENABLE_CAP_RV(rv);
	rv = acvp_enable_rsa_cap_sig_type_salt_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, RSA_SIG_TYPE_PKCS1PSS, MOD_RSA_2048, ACVP_RSA_SHA_256, RSA_SALT_SIGGEN_32);
	CHECK_ENABLE_CAP_RV(rv);
	rv = acvp_enable_rsa_cap_sig_type_salt_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, RSA_SIG_TYPE_PKCS1PSS, MOD_RSA_2048, ACVP_RSA_SHA_512, RSA_SALT_SIGGEN_64);
	CHECK_ENABLE_CAP_RV(rv);
	rv = acvp_enable_rsa_cap_sig_type_salt_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, RSA_SIG_TYPE_PKCS1PSS, MOD_RSA_3072, ACVP_RSA_SHA_224, RSA_SALT_SIGGEN_28);
	CHECK_ENABLE_CAP_RV(rv);
	rv = acvp_enable_rsa_cap_sig_type_salt_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, RSA_SIG_TYPE_PKCS1PSS, MOD_RSA_3072, ACVP_RSA_SHA_256, RSA_SALT_SIGGEN_32);
	CHECK_ENABLE_CAP_RV(rv);
	rv = acvp_enable_rsa_cap_sig_type_salt_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, RSA_SIG_TYPE_PKCS1PSS, MOD_RSA_3072, ACVP_RSA_SHA_512, RSA_SALT_SIGGEN_64);
	CHECK_ENABLE_CAP_RV(rv);
	rv = acvp_enable_rsa_cap_sig_type_salt_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, RSA_SIG_TYPE_PKCS1PSS, MOD_RSA_4096, ACVP_RSA_SHA_224, RSA_SALT_SIGGEN_28);
	CHECK_ENABLE_CAP_RV(rv);
	rv = acvp_enable_rsa_cap_sig_type_salt_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, RSA_SIG_TYPE_PKCS1PSS, MOD_RSA_4096, ACVP_RSA_SHA_256, RSA_SALT_SIGGEN_32);
	CHECK_ENABLE_CAP_RV(rv);
	rv = acvp_enable_rsa_cap_sig_type_salt_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN, RSA_SIG_TYPE_PKCS1PSS, MOD_RSA_4096, ACVP_RSA_SHA_512, RSA_SALT_SIGGEN_64);
	CHECK_ENABLE_CAP_RV(rv);

// RSA w/ sigType: PKCS1v1.5
    rv = acvp_enable_rsa_cap_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, ACVP_SIG_TYPE, RSA_SIG_TYPE_PKCS1V15);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, RSA_SIG_TYPE_PKCS1V15, MOD_RSA_2048, ACVP_RSA_SHA_224);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, RSA_SIG_TYPE_PKCS1V15, MOD_RSA_2048, ACVP_RSA_SHA_256);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, RSA_SIG_TYPE_PKCS1V15, MOD_RSA_2048, ACVP_RSA_SHA_512);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, RSA_SIG_TYPE_PKCS1V15, MOD_RSA_3072, ACVP_RSA_SHA_224);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, RSA_SIG_TYPE_PKCS1V15, MOD_RSA_3072, ACVP_RSA_SHA_256);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, RSA_SIG_TYPE_PKCS1V15, MOD_RSA_3072, ACVP_RSA_SHA_512);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, RSA_SIG_TYPE_PKCS1V15, MOD_RSA_4096, ACVP_RSA_SHA_224);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, RSA_SIG_TYPE_PKCS1V15, MOD_RSA_4096, ACVP_RSA_SHA_256);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, RSA_SIG_TYPE_PKCS1V15, MOD_RSA_4096, ACVP_RSA_SHA_512);
    CHECK_ENABLE_CAP_RV(rv);
// RSA w/ sigType: X9.31
    rv = acvp_enable_rsa_cap_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, ACVP_SIG_TYPE, RSA_SIG_TYPE_X931);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, RSA_SIG_TYPE_X931, MOD_RSA_2048, ACVP_RSA_SHA_224);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, RSA_SIG_TYPE_X931, MOD_RSA_2048, ACVP_RSA_SHA_256);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, RSA_SIG_TYPE_X931, MOD_RSA_2048, ACVP_RSA_SHA_512);
    CHECxxK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, RSA_SIG_TYPE_X931, MOD_RSA_3072, ACVP_RSA_SHA_224);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, RSA_SIG_TYPE_X931, MOD_RSA_3072, ACVP_RSA_SHA_256);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, RSA_SIG_TYPE_X931, MOD_RSA_3072, ACVP_RSA_SHA_512);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, RSA_SIG_TYPE_X931, MOD_RSA_4096, ACVP_RSA_SHA_224);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, RSA_SIG_TYPE_X931, MOD_RSA_4096, ACVP_RSA_SHA_256);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, RSA_SIG_TYPE_X931, MOD_RSA_4096, ACVP_RSA_SHA_512);
    CHECK_ENABLE_CAP_RV(rv);

// RSA w/ sigType: PKCS1PSS -- has saltSigVer, so new function is made as suggested by Ellie
    rv = acvp_enable_rsa_cap_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, ACVP_SIG_TYPE, RSA_SIG_TYPE_PKCS1PSS);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_salt_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, RSA_SIG_TYPE_PKCS1PSS, MOD_RSA_2048, ACVP_RSA_SHA_224, RSA_SALT_SIGGEN_28);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_salt_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, RSA_SIG_TYPE_PKCS1PSS, MOD_RSA_2048, ACVP_RSA_SHA_256, RSA_SALT_SIGGEN_32);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_salt_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, RSA_SIG_TYPE_PKCS1PSS, MOD_RSA_2048, ACVP_RSA_SHA_512, RSA_SALT_SIGGEN_64);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_salt_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, RSA_SIG_TYPE_PKCS1PSS, MOD_RSA_3072, ACVP_RSA_SHA_224, RSA_SALT_SIGGEN_28);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_salt_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, RSA_SIG_TYPE_PKCS1PSS, MOD_RSA_3072, ACVP_RSA_SHA_256, RSA_SALT_SIGGEN_32);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_salt_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, RSA_SIG_TYPE_PKCS1PSS, MOD_RSA_3072, ACVP_RSA_SHA_512, RSA_SALT_SIGGEN_64);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_salt_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, RSA_SIG_TYPE_PKCS1PSS, MOD_RSA_4096, ACVP_RSA_SHA_224, RSA_SALT_SIGGEN_28);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_salt_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, RSA_SIG_TYPE_PKCS1PSS, MOD_RSA_4096, ACVP_RSA_SHA_256, RSA_SALT_SIGGEN_32);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_rsa_cap_sig_type_salt_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGVER, RSA_SIG_TYPE_PKCS1PSS, MOD_RSA_4096, ACVP_RSA_SHA_512, RSA_SALT_SIGGEN_64);
    CHECK_ENABLE_CAP_RV(rv);*/

#if 0
    /*
     * We need to register all the crypto module capabilities that will be
     * validated.
     */
   rv = acvp_enable_sym_cipher_cap(ctx, ACVP_AES_GCM, ACVP_DIR_BOTH, ACVP_KO_NA, ACVP_IVGEN_SRC_INT, ACVP_IVGEN_MODE_821, &app_aes_handler_aead);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_prereq_cap(ctx, ACVP_AES_GCM, ACVP_SYM_PREREQ_AES, value);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_prereq_cap(ctx, ACVP_AES_GCM, ACVP_SYM_PREREQ_DRBG, value);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_GCM, ACVP_SYM_CIPH_KEYLEN, 128);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_GCM, ACVP_SYM_CIPH_KEYLEN, 192);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_GCM, ACVP_SYM_CIPH_KEYLEN, 256);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_GCM, ACVP_SYM_CIPH_TAGLEN, 96);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_GCM, ACVP_SYM_CIPH_TAGLEN, 128);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_GCM, ACVP_SYM_CIPH_IVLEN, 96);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_GCM, ACVP_SYM_CIPH_PTLEN, 0);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_GCM, ACVP_SYM_CIPH_PTLEN, 128);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_GCM, ACVP_SYM_CIPH_PTLEN, 136);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_GCM, ACVP_SYM_CIPH_AADLEN, 128);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_GCM, ACVP_SYM_CIPH_AADLEN, 136);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_GCM, ACVP_SYM_CIPH_AADLEN, 256);
   CHECK_ENABLE_CAP_RV(rv);

   /*
    * Enable AES-ECB 128,192,256 bit key
    */
   rv = acvp_enable_sym_cipher_cap(ctx, ACVP_AES_ECB, ACVP_DIR_BOTH, ACVP_KO_NA, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA, &app_aes_handler);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_ECB, ACVP_SYM_CIPH_KEYLEN, 128);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_ECB, ACVP_SYM_CIPH_KEYLEN, 192);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_ECB, ACVP_SYM_CIPH_KEYLEN, 256);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_ECB, ACVP_SYM_CIPH_PTLEN, 1536);
   CHECK_ENABLE_CAP_RV(rv);

   /*
    * Enable AES-CBC 128 bit key
    */
   rv = acvp_enable_sym_cipher_cap(ctx, ACVP_AES_CBC, ACVP_DIR_BOTH, ACVP_KO_NA, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA, &app_aes_handler);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CBC, ACVP_SYM_CIPH_KEYLEN, 128);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CBC, ACVP_SYM_CIPH_KEYLEN, 192);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CBC, ACVP_SYM_CIPH_KEYLEN, 256);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CBC, ACVP_SYM_CIPH_PTLEN, 128);
   CHECK_ENABLE_CAP_RV(rv);

   /*
    * Enable AES-CFB8 128,192,256 bit key
    */
   rv = acvp_enable_sym_cipher_cap(ctx, ACVP_AES_CFB8, ACVP_DIR_BOTH, ACVP_KO_NA, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA, &app_aes_handler);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CFB8, ACVP_SYM_CIPH_KEYLEN, 128);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CFB8, ACVP_SYM_CIPH_KEYLEN, 192);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CFB8, ACVP_SYM_CIPH_KEYLEN, 256);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CFB8, ACVP_SYM_CIPH_PTLEN, 256);
   CHECK_ENABLE_CAP_RV(rv);

   /*
    * Enable AES-CFB128 128,192,256 bit key
    */
   rv = acvp_enable_sym_cipher_cap(ctx, ACVP_AES_CFB128, ACVP_DIR_BOTH, ACVP_KO_NA, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA, &app_aes_handler);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CFB128, ACVP_SYM_CIPH_KEYLEN, 128);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CFB128, ACVP_SYM_CIPH_KEYLEN, 192);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CFB128, ACVP_SYM_CIPH_KEYLEN, 256);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CFB128, ACVP_SYM_CIPH_PTLEN, 1536);
   CHECK_ENABLE_CAP_RV(rv);

   /*
    * Enable AES-OFB 128, 192, 256 bit key
    */
   rv = acvp_enable_sym_cipher_cap(ctx, ACVP_AES_OFB, ACVP_DIR_BOTH, ACVP_KO_NA, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA, &app_aes_handler);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_OFB, ACVP_SYM_CIPH_KEYLEN, 128);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_OFB, ACVP_SYM_CIPH_KEYLEN, 192);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_OFB, ACVP_SYM_CIPH_KEYLEN, 256);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_OFB, ACVP_SYM_CIPH_PTLEN, 128);
   CHECK_ENABLE_CAP_RV(rv);

   /*
    * Register AES CCM capabilities
    */
   rv = acvp_enable_sym_cipher_cap(ctx, ACVP_AES_CCM, ACVP_DIR_BOTH, ACVP_KO_NA, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA, &app_aes_handler_aead);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_prereq_cap(ctx, ACVP_AES_CCM, ACVP_SYM_PREREQ_AES, value);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CCM, ACVP_SYM_CIPH_KEYLEN, 128);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CCM, ACVP_SYM_CIPH_KEYLEN, 192);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CCM, ACVP_SYM_CIPH_KEYLEN, 256);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CCM, ACVP_SYM_CIPH_PTLEN, 0);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CCM, ACVP_SYM_CIPH_PTLEN, 256);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CCM, ACVP_SYM_CIPH_TAGLEN, 128);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CCM, ACVP_SYM_CIPH_IVLEN, 56);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CCM, ACVP_SYM_CIPH_AADLEN, 0);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CCM, ACVP_SYM_CIPH_AADLEN, 65536);
   CHECK_ENABLE_CAP_RV(rv);
   #endif
#if 0
#ifdef ACVP_V04
   /*
    * Enable AES-CFB1 128,192,256 bit key
    */
   rv = acvp_enable_sym_cipher_cap(ctx, ACVP_AES_CFB1, ACVP_DIR_BOTH, ACVP_KO_NA, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA, &app_aes_handler);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CFB1, ACVP_SYM_CIPH_KEYLEN, 128);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CFB1, ACVP_SYM_CIPH_KEYLEN, 192);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CFB1, ACVP_SYM_CIPH_KEYLEN, 256);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CFB1, ACVP_SYM_CIPH_PTLEN, 1536);
   CHECK_ENABLE_CAP_RV(rv);

   /*
    * Enable AES keywrap for various key sizes and PT lengths
    * Note: this is with padding disabled, minimum PT length is 128 bits and must be
    *       a multiple of 64 bits.
    */
   rv = acvp_enable_sym_cipher_cap(ctx, ACVP_AES_KW, ACVP_DIR_BOTH, ACVP_KO_NA, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA, &app_aes_keywrap_handler);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_KW, ACVP_SYM_CIPH_KEYLEN, 128);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_KW, ACVP_SYM_CIPH_KEYLEN, 192);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_KW, ACVP_SYM_CIPH_KEYLEN, 256);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_KW, ACVP_SYM_CIPH_PTLEN, 512);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_KW, ACVP_SYM_CIPH_PTLEN, 192);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_KW, ACVP_SYM_CIPH_PTLEN, 128);
   CHECK_ENABLE_CAP_RV(rv);

   /*
    * Enable AES-CTR 128 bit key
    */
   rv = acvp_enable_sym_cipher_cap(ctx, ACVP_AES_CTR, ACVP_DIR_BOTH, ACVP_KO_NA, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA, &app_aes_handler);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CTR, ACVP_SYM_CIPH_KEYLEN, 128);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CTR, ACVP_SYM_CIPH_PTLEN, 128);
   CHECK_ENABLE_CAP_RV(rv);
#endif

   /*
    * Enable 3DES-ECB
    */
   rv = acvp_enable_sym_cipher_cap(ctx, ACVP_TDES_ECB, ACVP_DIR_BOTH, ACVP_KO_THREE, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA, &app_des_handler);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_ECB, ACVP_SYM_CIPH_KEYLEN, 192);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_ECB, ACVP_SYM_CIPH_PTLEN, 16*8*4);
   CHECK_ENABLE_CAP_RV(rv);

   /*
    * Enable 3DES-CBC
    */
   rv = acvp_enable_sym_cipher_cap(ctx, ACVP_TDES_CBC, ACVP_DIR_BOTH, ACVP_KO_THREE, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA, &app_des_handler);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CBC, ACVP_SYM_CIPH_KEYLEN, 192);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CBC, ACVP_SYM_CIPH_IVLEN, 192/3);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CBC, ACVP_SYM_CIPH_PTLEN, 64);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CBC, ACVP_SYM_CIPH_PTLEN, 64*2);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CBC, ACVP_SYM_CIPH_PTLEN, 64*3);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CBC, ACVP_SYM_CIPH_PTLEN, 64*12);
   CHECK_ENABLE_CAP_RV(rv);

#ifdef ACVP_V04
   /*
    * Enable 3DES-OFB
    */
   rv = acvp_enable_sym_cipher_cap(ctx, ACVP_TDES_OFB, ACVP_DIR_BOTH, ACVP_KO_THREE, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA, &app_des_handler);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_OFB, ACVP_SYM_CIPH_KEYLEN, 192);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_OFB, ACVP_SYM_CIPH_IVLEN, 192/3);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_OFB, ACVP_SYM_CIPH_PTLEN, 64);
   CHECK_ENABLE_CAP_RV(rv);

   /*
    * Enable 3DES-CFB64
    */
   rv = acvp_enable_sym_cipher_cap(ctx, ACVP_TDES_CFB64, ACVP_DIR_BOTH, ACVP_KO_THREE, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA, &app_des_handler);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CFB64, ACVP_SYM_CIPH_KEYLEN, 192);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CFB64, ACVP_SYM_CIPH_IVLEN, 192/3);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CFB64, ACVP_SYM_CIPH_PTLEN, 64 * 5);
   CHECK_ENABLE_CAP_RV(rv);

   /*
    * Enable 3DES-CFB8
    */
   rv = acvp_enable_sym_cipher_cap(ctx, ACVP_TDES_CFB8, ACVP_DIR_BOTH, ACVP_KO_THREE, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA, &app_des_handler);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CFB8, ACVP_SYM_CIPH_KEYLEN, 192);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CFB8, ACVP_SYM_CIPH_IVLEN, 192/3);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CFB8, ACVP_SYM_CIPH_PTLEN, 64);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CFB8, ACVP_SYM_CIPH_PTLEN, 64 * 4);
   CHECK_ENABLE_CAP_RV(rv);

   /*
    * Enable 3DES-CFB1
    */
   rv = acvp_enable_sym_cipher_cap(ctx, ACVP_TDES_CFB1, ACVP_DIR_BOTH, ACVP_KO_THREE, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA, &app_des_handler);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CFB1, ACVP_SYM_CIPH_KEYLEN, 192);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CFB1, ACVP_SYM_CIPH_IVLEN, 192/3);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CFB1, ACVP_SYM_CIPH_PTLEN, 64);
   CHECK_ENABLE_CAP_RV(rv);
#endif
   /*
    * Enable SHA-1 and SHA-2
    */

   rv = acvp_enable_hash_cap(ctx, ACVP_SHA1, &app_sha_handler);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_hash_cap_parm(ctx, ACVP_SHA1, ACVP_HASH_IN_BIT, 0);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_hash_cap_parm(ctx, ACVP_SHA1, ACVP_HASH_IN_EMPTY, 1);
   CHECK_ENABLE_CAP_RV(rv);

   rv = acvp_enable_hash_cap(ctx, ACVP_SHA224, &app_sha_handler);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_hash_cap_parm(ctx, ACVP_SHA224, ACVP_HASH_IN_BIT, 0);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_hash_cap_parm(ctx, ACVP_SHA224, ACVP_HASH_IN_EMPTY, 1);
   CHECK_ENABLE_CAP_RV(rv);

   rv = acvp_enable_hash_cap(ctx, ACVP_SHA256, &app_sha_handler);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_hash_cap_parm(ctx, ACVP_SHA256, ACVP_HASH_IN_BIT, 0);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_hash_cap_parm(ctx, ACVP_SHA256, ACVP_HASH_IN_EMPTY, 1);
   CHECK_ENABLE_CAP_RV(rv);

   rv = acvp_enable_hash_cap(ctx, ACVP_SHA384, &app_sha_handler);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_hash_cap_parm(ctx, ACVP_SHA384, ACVP_HASH_IN_BIT, 0);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_hash_cap_parm(ctx, ACVP_SHA384, ACVP_HASH_IN_EMPTY, 1);
   CHECK_ENABLE_CAP_RV(rv);

   rv = acvp_enable_hash_cap(ctx, ACVP_SHA512, &app_sha_handler);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_hash_cap_parm(ctx, ACVP_SHA512, ACVP_HASH_IN_BIT, 0);
   CHECK_ENABLE_CAP_RV(rv);
   rv = acvp_enable_hash_cap_parm(ctx, ACVP_SHA512, ACVP_HASH_IN_EMPTY, 1);
   CHECK_ENABLE_CAP_RV(rv);

#ifdef ACVP_V04

    /*
     * Enable CMAC
     */
    rv = acvp_enable_cmac_cap(ctx, ACVP_CMAC_AES_128, &app_cmac_handler);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_cmac_cap_parm(ctx, ACVP_CMAC_AES_128, ACVP_CMAC_BLK_DIVISIBLE_1, 1024);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_cmac_cap_parm(ctx, ACVP_CMAC_AES_128, ACVP_CMAC_BLK_NOT_DIVISIBLE_1, 2048);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_cmac_cap_parm(ctx, ACVP_CMAC_AES_128, ACVP_CMAC_IN_EMPTY, 1);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_cmac_cap_parm(ctx, ACVP_CMAC_AES_128, ACVP_CMAC_MACLEN, 64);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_cmac_cap_parm(ctx, ACVP_CMAC_AES_128, ACVP_CMAC_MACLEN, 256);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_prereq_cap(ctx, ACVP_CMAC_AES_128, CMAC_AES, value);
    CHECK_ENABLE_CAP_RV(rv);

    /*
     * Enable HMAC
     */
    rv = acvp_enable_hmac_cap(ctx, ACVP_HMAC_SHA1, &app_hmac_handler);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA1, ACVP_HMAC_KEYRANGE1_MIN, 0);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA1, ACVP_HMAC_KEYRANGE1_MAX, 65536);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA1, ACVP_HMAC_KEYRANGE2_MIN, 0);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA1, ACVP_HMAC_KEYRANGE2_MAX, 65536);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA1, ACVP_HMAC_KEYBLOCK, 0);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA1, ACVP_HMAC_IN_EMPTY, 1);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA1, ACVP_HMAC_MACLEN, 20);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_prereq_cap(ctx, ACVP_HMAC_SHA1, HMAC_SHA, value);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_hmac_cap(ctx, ACVP_HMAC_SHA2_224, &app_hmac_handler);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_224, ACVP_HMAC_KEYRANGE1_MIN, 0);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_224, ACVP_HMAC_KEYRANGE1_MAX, 65536);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_224, ACVP_HMAC_KEYRANGE2_MIN, 0);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_224, ACVP_HMAC_KEYRANGE2_MAX, 65536);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_224, ACVP_HMAC_KEYBLOCK, 0);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_224, ACVP_HMAC_IN_EMPTY, 1);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_224, ACVP_HMAC_MACLEN, 28);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_prereq_cap(ctx, ACVP_HMAC_SHA2_224, HMAC_SHA, value);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_hmac_cap(ctx, ACVP_HMAC_SHA2_256, &app_hmac_handler);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_256, ACVP_HMAC_KEYRANGE1_MIN, 0);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_256, ACVP_HMAC_KEYRANGE1_MAX, 65536);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_256, ACVP_HMAC_KEYRANGE2_MIN, 0);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_256, ACVP_HMAC_KEYRANGE2_MAX, 65536);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_256, ACVP_HMAC_KEYBLOCK, 0);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_256, ACVP_HMAC_IN_EMPTY, 1);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_256, ACVP_HMAC_MACLEN, 32);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_prereq_cap(ctx, ACVP_HMAC_SHA2_256, HMAC_SHA, value);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_hmac_cap(ctx, ACVP_HMAC_SHA2_384, &app_hmac_handler);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_384, ACVP_HMAC_KEYRANGE1_MIN, 0);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_384, ACVP_HMAC_KEYRANGE1_MAX, 65536);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_384, ACVP_HMAC_KEYRANGE2_MIN, 0);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_384, ACVP_HMAC_KEYRANGE2_MAX, 65536);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_384, ACVP_HMAC_KEYBLOCK, 0);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_384, ACVP_HMAC_IN_EMPTY, 1);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_384, ACVP_HMAC_MACLEN, 48);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_prereq_cap(ctx, ACVP_HMAC_SHA2_384, HMAC_SHA, value);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_hmac_cap(ctx, ACVP_HMAC_SHA2_512, &app_hmac_handler);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_512, ACVP_HMAC_KEYRANGE1_MIN, 0);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_512, ACVP_HMAC_KEYRANGE1_MAX, 65536);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_512, ACVP_HMAC_KEYRANGE2_MIN, 0);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_512, ACVP_HMAC_KEYRANGE2_MAX, 65536);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_512, ACVP_HMAC_KEYBLOCK, 0);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_512, ACVP_HMAC_IN_EMPTY, 1);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_512, ACVP_HMAC_MACLEN, 64);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_hmac_prereq_cap(ctx, ACVP_HMAC_SHA2_512, HMAC_SHA, value);
    CHECK_ENABLE_CAP_RV(rv);
#endif
#endif

#ifdef ACVP_NO_RUNTIME

#if 0  /* until drbg is supported by the server */
    /*
     * Register DRBG
     */
      ERR_load_crypto_strings() ;

      int fips_rc = FIPS_mode_set(1);
      if(!fips_rc) {
          (printf("Failed to enable FIPS mode.\n"));
          exit(1);
      }

    char value2[] = "123456";
    rv = acvp_enable_drbg_cap(ctx, ACVP_HASHDRBG, app_drbg_handler);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_drbg_cap_parm(ctx, ACVP_HASHDRBG, ACVP_DRBG_SHA_1,
                                    ACVP_DRBG_DER_FUNC_ENABLED, 0);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_prereq_cap(ctx, ACVP_HASHDRBG, ACVP_DRBG_SHA_1,
            DRBG_SHA, value);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_drbg_prereq_cap(ctx, ACVP_HASHDRBG, ACVP_DRBG_SHA_1,
            DRBG_AES, value2);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_cap_parm(ctx, ACVP_HASHDRBG, ACVP_DRBG_SHA_1,
            ACVP_DRBG_PRED_RESIST_ENABLED, 1);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_cap_parm(ctx, ACVP_HASHDRBG, ACVP_DRBG_SHA_1,
            ACVP_DRBG_RESEED_ENABLED, 1);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_cap_parm(ctx, ACVP_HASHDRBG, ACVP_DRBG_SHA_1,
            ACVP_DRBG_ENTROPY_LEN, 0);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_cap_parm(ctx, ACVP_HASHDRBG, ACVP_DRBG_SHA_1,
            ACVP_DRBG_NONCE_LEN, 0);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_cap_parm(ctx, ACVP_HASHDRBG, ACVP_DRBG_SHA_1,
            ACVP_DRBG_PERSO_LEN, 0);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_cap_parm(ctx, ACVP_HASHDRBG, ACVP_DRBG_SHA_1,
            ACVP_DRBG_ADD_IN_LEN, 0);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_cap_parm(ctx, ACVP_HASHDRBG, ACVP_DRBG_SHA_1,
            ACVP_DRBG_RET_BITS_LEN, 512);
    CHECK_ENABLE_CAP_RV(rv);


    //ACVP_HMACDRBG

    rv = acvp_enable_drbg_cap(ctx, ACVP_HMACDRBG, app_drbg_handler);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_cap_parm(ctx, ACVP_HMACDRBG, ACVP_DRBG_SHA_224,
                                    ACVP_DRBG_DER_FUNC_ENABLED, 0);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_prereq_cap(ctx, ACVP_HMACDRBG, ACVP_DRBG_SHA_224,
            DRBG_SHA, value);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_drbg_prereq_cap(ctx, ACVP_HMACDRBG, ACVP_DRBG_SHA_224,
            DRBG_AES, value2);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_cap_parm(ctx, ACVP_HMACDRBG, ACVP_DRBG_SHA_224,
            ACVP_DRBG_PRED_RESIST_ENABLED, 1);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_cap_parm(ctx, ACVP_HMACDRBG, ACVP_DRBG_SHA_224,
            ACVP_DRBG_RESEED_ENABLED, 1);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_cap_parm(ctx, ACVP_HMACDRBG, ACVP_DRBG_SHA_224,
            ACVP_DRBG_ENTROPY_LEN, 0);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_cap_parm(ctx, ACVP_HMACDRBG, ACVP_DRBG_SHA_224,
            ACVP_DRBG_NONCE_LEN, 0);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_cap_parm(ctx, ACVP_HMACDRBG, ACVP_DRBG_SHA_224,
            ACVP_DRBG_PERSO_LEN, 0);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_cap_parm(ctx, ACVP_HMACDRBG, ACVP_DRBG_SHA_224,
            ACVP_DRBG_ADD_IN_LEN, 0);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_cap_parm(ctx, ACVP_HMACDRBG, ACVP_DRBG_SHA_224,
            ACVP_DRBG_RET_BITS_LEN, 512);
    CHECK_ENABLE_CAP_RV(rv);

    //Add length range
    rv = acvp_enable_drbg_length_cap(ctx, ACVP_HMACDRBG, ACVP_DRBG_SHA_224,
            ACVP_DRBG_ENTROPY_LEN, (int)0, (int)128,(int) 256);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_length_cap(ctx, ACVP_HMACDRBG, ACVP_DRBG_SHA_224,
            ACVP_DRBG_NONCE_LEN, (int)0, (int)128,(int) 256);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_length_cap(ctx, ACVP_HMACDRBG, ACVP_DRBG_SHA_224,
            ACVP_DRBG_PERSO_LEN, (int)0, (int)128,(int) 256);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_length_cap(ctx, ACVP_HMACDRBG, ACVP_DRBG_SHA_224,
            ACVP_DRBG_PERSO_LEN, (int)0, (int)128,(int) 256);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_length_cap(ctx, ACVP_HMACDRBG, ACVP_DRBG_SHA_224,
            ACVP_DRBG_ADD_IN_LEN, (int)0, (int)128,(int) 256);
    CHECK_ENABLE_CAP_RV(rv);

    // ACVP_CTRDRBG
    rv = acvp_enable_drbg_cap(ctx, ACVP_CTRDRBG, app_drbg_handler);
    CHECK_ENABLE_CAP_RV(rv);

    //Add length range
    rv = acvp_enable_drbg_length_cap(ctx, ACVP_CTRDRBG, ACVP_DRBG_AES_128,
            ACVP_DRBG_ENTROPY_LEN, (int)0, (int)128,(int) 256);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_length_cap(ctx, ACVP_CTRDRBG, ACVP_DRBG_AES_128,
            ACVP_DRBG_NONCE_LEN, (int)0, (int)128,(int) 256);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_length_cap(ctx, ACVP_CTRDRBG, ACVP_DRBG_AES_128,
            ACVP_DRBG_PERSO_LEN, (int)0, (int)128,(int) 256);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_length_cap(ctx, ACVP_CTRDRBG, ACVP_DRBG_AES_128,
            ACVP_DRBG_PERSO_LEN, (int)0, (int)128,(int) 256);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_length_cap(ctx, ACVP_CTRDRBG, ACVP_DRBG_AES_128,
            ACVP_DRBG_ADD_IN_LEN, (int)0, (int)128,(int) 256);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_cap_parm(ctx, ACVP_CTRDRBG, ACVP_DRBG_AES_128,
                                    ACVP_DRBG_DER_FUNC_ENABLED, 1);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_prereq_cap(ctx, ACVP_CTRDRBG, ACVP_DRBG_AES_128,
            DRBG_SHA, value);
    CHECK_ENABLE_CAP_RV(rv);
    rv = acvp_enable_drbg_prereq_cap(ctx, ACVP_CTRDRBG, ACVP_DRBG_AES_128,
            DRBG_AES, value2);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_cap_parm(ctx, ACVP_CTRDRBG, ACVP_DRBG_AES_128,
            ACVP_DRBG_PRED_RESIST_ENABLED, 1);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_cap_parm(ctx, ACVP_CTRDRBG, ACVP_DRBG_AES_128,
            ACVP_DRBG_RESEED_ENABLED, 1);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_cap_parm(ctx, ACVP_CTRDRBG, ACVP_DRBG_AES_128,
            ACVP_DRBG_ENTROPY_LEN, 0);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_cap_parm(ctx, ACVP_CTRDRBG, ACVP_DRBG_AES_128,
            ACVP_DRBG_NONCE_LEN, 0);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_cap_parm(ctx, ACVP_CTRDRBG, ACVP_DRBG_AES_128,
            ACVP_DRBG_PERSO_LEN, 0);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_cap_parm(ctx, ACVP_CTRDRBG, ACVP_DRBG_AES_128,
            ACVP_DRBG_ADD_IN_LEN, 0);
    CHECK_ENABLE_CAP_RV(rv);

    rv = acvp_enable_drbg_cap_parm(ctx, ACVP_CTRDRBG, ACVP_DRBG_AES_128,
            ACVP_DRBG_RET_BITS_LEN, 512);
    CHECK_ENABLE_CAP_RV(rv);

#endif
#endif

    /*
     * Now that we have a test session, we register with
     * the server to advertise our capabilities and receive
     * the KAT vector sets the server demands that we process.
     */
    rv = acvp_register(ctx);
    if (rv != ACVP_SUCCESS) {
        printf("Failed to register with ACVP server (rv=%d)\n", rv);
        exit(1);
    }

    /*
     * Now we process the test cases given to us during
     * registration earlier.
     */
    rv = acvp_process_tests(ctx);
    if (rv != ACVP_SUCCESS) {
        printf("Failed to process vectors (%d)\n", rv);
        exit(1);
    }

    printf("\nTests complete, checking results...\n");
    rv = acvp_check_test_results(ctx);
    if (rv != ACVP_SUCCESS) {
        printf("Unable to retrieve test results (%d)\n", rv);
        exit(1);
    }

    /*
     * Finally, we free the test session context and cleanup
     */
    rv = acvp_free_test_session(ctx);
    if (rv != ACVP_SUCCESS) {
        printf("Failed to free ACVP context\n");
        exit(1);
    }
    acvp_cleanup();

    return (0);
}

static ACVP_RESULT app_des_handler(ACVP_TEST_CASE *test_case)
{
    ACVP_SYM_CIPHER_TC      *tc;
    const EVP_CIPHER        *cipher;
    int ct_len, pt_len;
    unsigned char *iv = 0;

    if (!test_case) {
        return ACVP_INVALID_ARG;
    }

    tc = test_case->tc.symmetric;

    /*
     * We only support 3 key DES
     */
    if (tc->key_len != 192) {
	printf("Unsupported DES key length\n");
	return ACVP_NO_CAP;
    }

    /* Begin encrypt code section */
    if (cipher_ctx.cipher == NULL) {
	EVP_CIPHER_CTX_init(&cipher_ctx);
    }

    switch (tc->cipher) {
    case ACVP_TDES_ECB:
	cipher = EVP_des_ede3_ecb();
	break;
    case ACVP_TDES_CBC:
	iv = tc->iv;
	cipher = EVP_des_ede3_cbc();
	break;
    case ACVP_TDES_OFB:
	iv = tc->iv;
	cipher = EVP_des_ede3_ofb();
	break;
    case ACVP_TDES_CFB64:
	iv = tc->iv;
	cipher = EVP_des_ede3_cfb64();
	break;
    case ACVP_TDES_CFB8:
	iv = tc->iv;
	cipher = EVP_des_ede3_cfb8();
	break;
    case ACVP_TDES_CFB1:
	iv = tc->iv;
	cipher = EVP_des_ede3_cfb1();
	break;
    default:
	printf("Error: Unsupported DES mode requested by ACVP server\n");
	return ACVP_NO_CAP;
	break;
    }

    /* If Monte Carlo we need to be able to init and then update
     * one thousand times before we complete each iteration.
     */
    if (tc->test_type == ACVP_SYM_TEST_TYPE_MCT) {
        if (tc->direction == ACVP_DIR_ENCRYPT) {
            if (tc->mct_index == 0) {
	        EVP_EncryptInit_ex(&cipher_ctx, cipher, NULL, tc->key, iv);
        	EVP_CIPHER_CTX_set_padding(&cipher_ctx, 0);
            }
	    EVP_EncryptUpdate(&cipher_ctx, tc->ct, &ct_len, tc->pt, tc->pt_len);
	    tc->ct_len = ct_len;
        } else if (tc->direction == ACVP_DIR_DECRYPT) {
            if (tc->mct_index == 0) {
	        EVP_DecryptInit_ex(&cipher_ctx, cipher, NULL, tc->key, iv);
        	EVP_CIPHER_CTX_set_padding(&cipher_ctx, 0);
            }
	    EVP_DecryptUpdate(&cipher_ctx, tc->pt, &pt_len, tc->ct, tc->ct_len);
	    tc->pt_len = pt_len;
        } else {
            printf("Unsupported direction\n");
	    return ACVP_UNSUPPORTED_OP;
        }
        if (tc->mct_index == 9999) {
            EVP_CIPHER_CTX_cleanup(&cipher_ctx);
        }
    } else {
        if (tc->direction == ACVP_DIR_ENCRYPT) {
	    EVP_EncryptInit_ex(&cipher_ctx, cipher, NULL, tc->key, iv);
	    EVP_CIPHER_CTX_set_padding(&cipher_ctx, 0);
	    EVP_EncryptUpdate(&cipher_ctx, tc->ct, &ct_len, tc->pt, tc->pt_len);
	    tc->ct_len = ct_len;
	    EVP_EncryptFinal_ex(&cipher_ctx, tc->ct + ct_len, &ct_len);
	    tc->ct_len += ct_len;
        } else if (tc->direction == ACVP_DIR_DECRYPT) {
	    EVP_DecryptInit_ex(&cipher_ctx, cipher, NULL, tc->key, iv);
	    EVP_CIPHER_CTX_set_padding(&cipher_ctx, 0);
	    EVP_DecryptUpdate(&cipher_ctx, tc->pt, &pt_len, tc->ct, tc->ct_len);
	    tc->pt_len = pt_len;
	    EVP_DecryptFinal_ex(&cipher_ctx, tc->pt + pt_len, &pt_len);
	    tc->pt_len += pt_len;
        } else {
            printf("Unsupported direction\n");
	    return ACVP_UNSUPPORTED_OP;
        }

        EVP_CIPHER_CTX_cleanup(&cipher_ctx);
    }

    return ACVP_SUCCESS;
}


static ACVP_RESULT app_aes_handler(ACVP_TEST_CASE *test_case)
{
    ACVP_SYM_CIPHER_TC      *tc;
    const EVP_CIPHER        *cipher;
    int ct_len, pt_len;
    unsigned char *iv = 0;

    if (!test_case) {
        return ACVP_INVALID_ARG;
    }

    tc = test_case->tc.symmetric;

    /* Begin encrypt code section */
    if ((cipher_ctx.cipher == NULL) || (tc->test_type != ACVP_SYM_TEST_TYPE_MCT)) {
	EVP_CIPHER_CTX_init(&cipher_ctx);
    }

    switch (tc->cipher) {
    case ACVP_AES_ECB:
	switch (tc->key_len) {
	case 128:
	    cipher = EVP_aes_128_ecb();
	    break;
	case 192:
	    cipher = EVP_aes_192_ecb();
	    break;
	case 256:
	    cipher = EVP_aes_256_ecb();
	    break;
	default:
	    printf("Unsupported AES key length\n");
	    return ACVP_NO_CAP;
	    break;
	}
	break;
    case ACVP_AES_CTR:
	iv = tc->iv;
	switch (tc->key_len) {
	case 128:
	    cipher = EVP_aes_128_ctr();
	    break;
	case 192:
	    cipher = EVP_aes_192_ctr();
	    break;
	case 256:
	    cipher = EVP_aes_256_ctr();
	    break;
	default:
	    printf("Unsupported AES key length\n");
	    return ACVP_NO_CAP;
	    break;
	}
	break;
    case ACVP_AES_CFB1:
	iv = tc->iv;
	switch (tc->key_len) {
	case 128:
	    cipher = EVP_aes_128_cfb1();
	    break;
	case 192:
	    cipher = EVP_aes_192_cfb1();
	    break;
	case 256:
	    cipher = EVP_aes_256_cfb1();
	    break;
	default:
	    printf("Unsupported AES key length\n");
	    return ACVP_NO_CAP;
	    break;
	}
	break;
    case ACVP_AES_CFB8:
	iv = tc->iv;
	switch (tc->key_len) {
	case 128:
	    cipher = EVP_aes_128_cfb8();
	    break;
	case 192:
	    cipher = EVP_aes_192_cfb8();
	    break;
	case 256:
	    cipher = EVP_aes_256_cfb8();
	    break;
	default:
	    printf("Unsupported AES key length\n");
	    return ACVP_NO_CAP;
	    break;
	}
	break;
    case ACVP_AES_CFB128:
	iv = tc->iv;
	switch (tc->key_len) {
	case 128:
	    cipher = EVP_aes_128_cfb128();
	    break;
	case 192:
	    cipher = EVP_aes_192_cfb128();
	    break;
	case 256:
	    cipher = EVP_aes_256_cfb128();
	    break;
	default:
	    printf("Unsupported AES key length\n");
	    return ACVP_NO_CAP;
	    break;
	}
	break;
    case ACVP_AES_OFB:
	iv = tc->iv;
	switch (tc->key_len) {
	case 128:
	    cipher = EVP_aes_128_ofb();
	    break;
	case 192:
	    cipher = EVP_aes_192_ofb();
	    break;
	case 256:
	    cipher = EVP_aes_256_ofb();
	    break;
	default:
	    printf("Unsupported AES key length\n");
	    return ACVP_NO_CAP;
	    break;
	}
	break;
    case ACVP_AES_CBC:
	iv = tc->iv;
	switch (tc->key_len) {
	case 128:
	    cipher = EVP_aes_128_cbc();
	    break;
	case 192:
	    cipher = EVP_aes_192_cbc();
	    break;
	case 256:
	    cipher = EVP_aes_256_cbc();
	    break;
	default:
	    printf("Unsupported AES key length\n");
	    return ACVP_NO_CAP;
	    break;
	}
	break;
    default:
	printf("Error: Unsupported AES mode requested by ACVP server\n");
	return ACVP_NO_CAP;
	break;
    }

    /* If Monte Carlo we need to be able to init and then update
     * one thousand times before we complete each iteration.
     */
    if (tc->test_type == ACVP_SYM_TEST_TYPE_MCT) {
        if (tc->direction == ACVP_DIR_ENCRYPT) {
            if (tc->mct_index == 0) {
	        EVP_EncryptInit_ex(&cipher_ctx, cipher, NULL, tc->key, iv);
		EVP_CIPHER_CTX_set_padding(&cipher_ctx, 0);
            }
	    EVP_EncryptUpdate(&cipher_ctx, tc->ct, &ct_len, tc->pt, tc->pt_len);
	    tc->ct_len = ct_len;
        } else if (tc->direction == ACVP_DIR_DECRYPT) {
            if (tc->mct_index == 0) {
	        EVP_DecryptInit_ex(&cipher_ctx, cipher, NULL, tc->key, iv);
	        EVP_CIPHER_CTX_set_padding(&cipher_ctx, 0);
            }
	    EVP_DecryptUpdate(&cipher_ctx, tc->pt, &pt_len, tc->ct, tc->ct_len);
	    tc->pt_len = pt_len;
        } else {
            printf("Unsupported direction\n");
	    return ACVP_UNSUPPORTED_OP;
        }
        if (tc->mct_index == 999) {
            EVP_CIPHER_CTX_cleanup(&cipher_ctx);
        }

    } else {
        if (tc->direction == ACVP_DIR_ENCRYPT) {
	    EVP_EncryptInit_ex(&cipher_ctx, cipher, NULL, tc->key, iv);
   	    EVP_CIPHER_CTX_set_padding(&cipher_ctx, 0);
	    EVP_EncryptUpdate(&cipher_ctx, tc->ct, &ct_len, tc->pt, tc->pt_len);
	    tc->ct_len = ct_len;
	    EVP_EncryptFinal_ex(&cipher_ctx, tc->ct + ct_len, &ct_len);
	    tc->ct_len += ct_len;
        } else if (tc->direction == ACVP_DIR_DECRYPT) {
	    EVP_DecryptInit_ex(&cipher_ctx, cipher, NULL, tc->key, iv);
	    EVP_CIPHER_CTX_set_padding(&cipher_ctx, 0);
	    EVP_DecryptUpdate(&cipher_ctx, tc->pt, &pt_len, tc->ct, tc->ct_len);
	    tc->pt_len = pt_len;
	    EVP_DecryptFinal_ex(&cipher_ctx, tc->pt + pt_len, &pt_len);
	    tc->pt_len += pt_len;
        } else {
            printf("Unsupported direction\n");
	    return ACVP_UNSUPPORTED_OP;
       }
       EVP_CIPHER_CTX_cleanup(&cipher_ctx);
    }

    return ACVP_SUCCESS;
}

static ACVP_RESULT app_aes_keywrap_handler(ACVP_TEST_CASE *test_case)
{
    ACVP_SYM_CIPHER_TC      *tc;
    EVP_CIPHER_CTX cipher_ctx;
    const EVP_CIPHER        *cipher;
    int c_len;

    if (!test_case) {
        return ACVP_INVALID_ARG;
    }

    tc = test_case->tc.symmetric;

    /* Begin encrypt code section */
    EVP_CIPHER_CTX_init(&cipher_ctx);

    switch (tc->cipher) {
    case ACVP_AES_KW:
	switch (tc->key_len) {
	case 128:
	    cipher = EVP_aes_128_wrap();
	    break;
	case 192:
	    cipher = EVP_aes_192_wrap();
	    break;
	case 256:
	    cipher = EVP_aes_256_wrap();
	    break;
	default:
	    printf("Unsupported AES keywrap key length\n");
	    return ACVP_NO_CAP;
	    break;
	}
	break;
    default:
	printf("Error: Unsupported AES keywrap mode requested by ACVP server\n");
	return ACVP_NO_CAP;
	break;
    }


    if (tc->direction == ACVP_DIR_ENCRYPT) {
	EVP_CIPHER_CTX_set_flags(&cipher_ctx, EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);
	EVP_CipherInit_ex(&cipher_ctx, cipher, NULL, tc->key, NULL, 1);
	c_len = EVP_Cipher(&cipher_ctx, tc->ct, tc->pt, tc->pt_len);
	if (c_len <= 0) {
	    printf("Error: key wrap operation failed (%d)\n", c_len);
	    return ACVP_CRYPTO_MODULE_FAIL;
	} else {
	    tc->ct_len = c_len;
	}
    } else if (tc->direction == ACVP_DIR_DECRYPT) {
	EVP_CIPHER_CTX_set_flags(&cipher_ctx, EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);
	EVP_CipherInit_ex(&cipher_ctx, cipher, NULL, tc->key, NULL, 0);
	c_len = EVP_Cipher(&cipher_ctx, tc->pt, tc->ct, tc->ct_len + 8);
	if (c_len <= 0) {
	    printf("Error: key wrap operation failed (%d)\n", c_len);
	    return ACVP_CRYPTO_MODULE_FAIL;
	}
    } else {
        printf("Unsupported direction\n");
        return ACVP_UNSUPPORTED_OP;
    }

    EVP_CIPHER_CTX_cleanup(&cipher_ctx);

    return ACVP_SUCCESS;
}

/*
 * This fuction is invoked by libacvp when an AES crypto
 * operation is needed from the crypto module being
 * validated.  This is a callback provided to libacvp when
 * acvp_enable_capability() is invoked to register the
 * AES-GCM capabilitiy with libacvp.  libacvp will in turn
 * invoke this function when it needs to process an AES-GCM
 * test case.
 */
//TODO: I have mixed feelings on returing ACVP_RESULT.  This is
//      application layer code outside of libacvp.  Should we
//      return a simple pass/fail?  Should we provide a separate
//      enum that applications can use?
static ACVP_RESULT app_aes_handler_aead(ACVP_TEST_CASE *test_case)
{
    ACVP_SYM_CIPHER_TC      *tc;
    EVP_CIPHER_CTX cipher_ctx;
    const EVP_CIPHER        *cipher;
    unsigned char iv_fixed[4] = {1,2,3,4};
    int rv;

    if (!test_case) {
        return ACVP_INVALID_ARG;
    }

    tc = test_case->tc.symmetric;

    if (tc->direction != ACVP_DIR_ENCRYPT && tc->direction != ACVP_DIR_DECRYPT) {
        printf("Unsupported direction\n");
        return ACVP_UNSUPPORTED_OP;
    }

    /* Begin encrypt code section */
    EVP_CIPHER_CTX_init(&cipher_ctx);

    /* Validate key length and assign OpenSSL EVP cipher */
    switch (tc->cipher) {
    case ACVP_AES_GCM:
	switch (tc->key_len) {
	case 128:
	    cipher = EVP_aes_128_gcm();
	    break;
	case 192:
	    cipher = EVP_aes_192_gcm();
	    break;
	case 256:
	    cipher = EVP_aes_256_gcm();
	    break;
	default:
	    printf("Unsupported AES-GCM key length\n");
	    return ACVP_UNSUPPORTED_OP;
	}
	if (tc->direction == ACVP_DIR_ENCRYPT) {
	    EVP_CIPHER_CTX_set_flags(&cipher_ctx, EVP_CIPH_FLAG_NON_FIPS_ALLOW);
	    EVP_CipherInit(&cipher_ctx, cipher, NULL, NULL, 1);
	    EVP_CIPHER_CTX_ctrl(&cipher_ctx, EVP_CTRL_GCM_SET_IVLEN, tc->iv_len, 0);
	    EVP_CipherInit(&cipher_ctx, NULL, tc->key, NULL, 1);

	    EVP_CIPHER_CTX_ctrl(&cipher_ctx, EVP_CTRL_GCM_SET_IV_FIXED, 4, iv_fixed);
	    if (!EVP_CIPHER_CTX_ctrl(&cipher_ctx, EVP_CTRL_GCM_IV_GEN, tc->iv_len, tc->iv)) {
		printf("acvp_aes_encrypt: iv gen error\n");
		return ACVP_CRYPTO_MODULE_FAIL;
	    }
	    if (tc->aad_len) {
		EVP_Cipher(&cipher_ctx, NULL, tc->aad, tc->aad_len);
	    }
	    EVP_Cipher(&cipher_ctx, tc->ct, tc->pt, tc->pt_len);
	    EVP_Cipher(&cipher_ctx, NULL, NULL, 0);
	    EVP_CIPHER_CTX_ctrl(&cipher_ctx, EVP_CTRL_GCM_GET_TAG, tc->tag_len, tc->tag);
	} else if (tc->direction == ACVP_DIR_DECRYPT) {
	    EVP_CIPHER_CTX_set_flags(&cipher_ctx, EVP_CIPH_FLAG_NON_FIPS_ALLOW);
	    EVP_CipherInit_ex(&cipher_ctx, cipher, NULL, tc->key, NULL, 0);
	    EVP_CIPHER_CTX_ctrl(&cipher_ctx, EVP_CTRL_GCM_SET_IVLEN, tc->iv_len, 0);
	    EVP_CIPHER_CTX_ctrl(&cipher_ctx, EVP_CTRL_GCM_SET_IV_FIXED, -1, tc->iv);
	    if(!EVP_CIPHER_CTX_ctrl(&cipher_ctx, EVP_CTRL_GCM_IV_GEN, tc->iv_len, tc->iv)) {
		printf("\nFailed to set IV");;
		return ACVP_CRYPTO_MODULE_FAIL;
	    }
	    if (tc->aad_len) {
		/*
		 * Set dummy tag before processing AAD.  Otherwise the AAD can
		 * not be processed.
		 */
		EVP_CIPHER_CTX_ctrl(&cipher_ctx, EVP_CTRL_GCM_SET_TAG, tc->tag_len, tc->tag);
		EVP_Cipher(&cipher_ctx, NULL, tc->aad, tc->aad_len);
	    }
	    /*
	     * Set the tag when decrypting
	     */
	    EVP_CIPHER_CTX_ctrl(&cipher_ctx, EVP_CTRL_GCM_SET_TAG, tc->tag_len, tc->tag);

	    /*
	     * Decrypt the CT
	     */
	    EVP_Cipher(&cipher_ctx, tc->pt, tc->ct, tc->pt_len);
	    /*
	     * Check the tag
	     */
	    rv = EVP_Cipher(&cipher_ctx, NULL, NULL, 0);
	    if (rv) {
		return ACVP_CRYPTO_TAG_FAIL;
	    }
	}
	break;
    case ACVP_AES_CCM:
	switch (tc->key_len) {
	case 128:
	    cipher = EVP_aes_128_ccm();
	    break;
	case 192:
	    cipher = EVP_aes_192_ccm();
	    break;
	case 256:
	    cipher = EVP_aes_256_ccm();
	    break;
	default:
	    printf("Unsupported AES-CCM key length\n");
	    return ACVP_UNSUPPORTED_OP;
	}
	if (tc->direction == ACVP_DIR_ENCRYPT) {
	    EVP_CipherInit(&cipher_ctx, cipher, NULL, NULL, 1);
	    EVP_CIPHER_CTX_ctrl(&cipher_ctx, EVP_CTRL_CCM_SET_IVLEN, tc->iv_len, 0);
	    EVP_CIPHER_CTX_ctrl(&cipher_ctx, EVP_CTRL_CCM_SET_TAG, tc->tag_len, 0);
	    EVP_CipherInit(&cipher_ctx, NULL, tc->key, tc->iv, 1);
	    EVP_Cipher(&cipher_ctx, NULL, NULL, tc->pt_len);
	    EVP_Cipher(&cipher_ctx, NULL, tc->aad, tc->aad_len);
	    EVP_Cipher(&cipher_ctx, tc->ct, tc->pt, tc->pt_len);
	    EVP_CIPHER_CTX_ctrl(&cipher_ctx, EVP_CTRL_CCM_GET_TAG, tc->tag_len, tc->ct + tc->ct_len);
	    tc->ct_len += tc->tag_len;
	} else if (tc->direction == ACVP_DIR_DECRYPT) {
	    EVP_CipherInit(&cipher_ctx, cipher, NULL, NULL, 0);
	    EVP_CIPHER_CTX_ctrl(&cipher_ctx, EVP_CTRL_CCM_SET_IVLEN, tc->iv_len, 0);
	    EVP_CIPHER_CTX_ctrl(&cipher_ctx, EVP_CTRL_CCM_SET_TAG, tc->tag_len, tc->ct + tc->pt_len);
	    EVP_CipherInit(&cipher_ctx, NULL, tc->key, tc->iv, 0);
	    EVP_Cipher(&cipher_ctx, NULL, NULL, tc->pt_len);
	    EVP_Cipher(&cipher_ctx, NULL, tc->aad, tc->aad_len);
	    /*
	     * Decrypt and check the tag
	     */
	    rv = EVP_Cipher(&cipher_ctx, tc->pt, tc->ct, tc->pt_len);
	    if (rv < 0) {
		return ACVP_CRYPTO_TAG_FAIL;
	    }
	}
	break;
    default:
	printf("Error: Unsupported AES AEAD mode requested by ACVP server\n");
	return ACVP_NO_CAP;
	break;
    }

    EVP_CIPHER_CTX_cleanup(&cipher_ctx);

    return ACVP_SUCCESS;
}

static ACVP_RESULT app_sha_handler(ACVP_TEST_CASE *test_case)
{
    ACVP_HASH_TC	*tc;
    const EVP_MD	*md;
    EVP_MD_CTX          md_ctx;

    if (!test_case) {
        return ACVP_INVALID_ARG;
    }

    tc = test_case->tc.hash;

    switch (tc->cipher) {
    case ACVP_SHA1:
	md = EVP_sha1();
	break;
    case ACVP_SHA224:
	md = EVP_sha224();
	break;
    case ACVP_SHA256:
	md = EVP_sha256();
	break;
    case ACVP_SHA384:
	md = EVP_sha384();
	break;
    case ACVP_SHA512:
	md = EVP_sha512();
	break;
    default:
	printf("Error: Unsupported hash algorithm requested by ACVP server\n");
	return ACVP_NO_CAP;
	break;
    }

    EVP_MD_CTX_init(&md_ctx);

    /* If Monte Carlo we need to be able to init and then update
     * one thousand times before we complete each iteration.
     */
    if (tc->test_type == ACVP_HASH_TEST_TYPE_MCT) {

        if (!EVP_DigestInit_ex(&md_ctx, md, NULL)) {
            printf("\nCrypto module error, EVP_DigestInit_ex failed\n");
	    return ACVP_CRYPTO_MODULE_FAIL;
        }
        if (!EVP_DigestUpdate(&md_ctx, tc->m1, tc->msg_len)) {
	    printf("\nCrypto module error, EVP_DigestUpdate failed\n");
	    return ACVP_CRYPTO_MODULE_FAIL;
        }
	if (!EVP_DigestUpdate(&md_ctx, tc->m2, tc->msg_len)) {
	    printf("\nCrypto module error, EVP_DigestUpdate failed\n");
	    return ACVP_CRYPTO_MODULE_FAIL;
        }
	if (!EVP_DigestUpdate(&md_ctx, tc->m3, tc->msg_len)) {
	    printf("\nCrypto module error, EVP_DigestUpdate failed\n");
	    return ACVP_CRYPTO_MODULE_FAIL;
        }
	if (!EVP_DigestFinal(&md_ctx, tc->md, &tc->md_len)) {
	    printf("\nCrypto module error, EVP_DigestFinal failed\n");
	    return ACVP_CRYPTO_MODULE_FAIL;
        }

   } else {
        if (!EVP_DigestInit_ex(&md_ctx, md, NULL)) {
            printf("\nCrypto module error, EVP_DigestInit_ex failed\n");
	    return ACVP_CRYPTO_MODULE_FAIL;
        }

	if (!EVP_DigestUpdate(&md_ctx, tc->msg, tc->msg_len)) {
	    printf("\nCrypto module error, EVP_DigestUpdate failed\n");
	    return ACVP_CRYPTO_MODULE_FAIL;
        }
	if (!EVP_DigestFinal(&md_ctx, tc->md, &tc->md_len)) {
	    printf("\nCrypto module error, EVP_DigestFinal failed\n");
	    return ACVP_CRYPTO_MODULE_FAIL;
        }
	EVP_MD_CTX_cleanup(&md_ctx);
   }

    return ACVP_SUCCESS;
}

static ACVP_RESULT app_hmac_handler(ACVP_TEST_CASE *test_case)
{
    ACVP_HMAC_TC	*tc;
    const EVP_MD	*md;
    HMAC_CTX       hmac_ctx;
    int msg_len;

    if (!test_case) {
        return ACVP_INVALID_ARG;
    }

    tc = test_case->tc.hmac;

    switch (tc->cipher) {
    case ACVP_HMAC_SHA1:
      md = EVP_sha1();
      break;
    case ACVP_HMAC_SHA2_224:
      md = EVP_sha224();
      break;
    case ACVP_HMAC_SHA2_256:
      md = EVP_sha256();
      break;
    case ACVP_HMAC_SHA2_384:
      md = EVP_sha384();
      break;
    case ACVP_HMAC_SHA2_512:
      md = EVP_sha512();
      break;
    default:
    	printf("Error: Unsupported hash algorithm requested by ACVP server\n");
    	return ACVP_NO_CAP;
    	break;
    }

    HMAC_CTX_init(&hmac_ctx);
    msg_len = tc->msg_len;

    if (!HMAC_Init_ex(&hmac_ctx, tc->key, tc->key_len, md, NULL)) {
        printf("\nCrypto module error, HMAC_Init_ex failed\n");
        return ACVP_CRYPTO_MODULE_FAIL;
    }

    if (!HMAC_Update(&hmac_ctx, tc->msg, msg_len)) {
        printf("\nCrypto module error, HMAC_Update failed\n");
        return ACVP_CRYPTO_MODULE_FAIL;
    }

    if (!HMAC_Final(&hmac_ctx, tc->mac, &tc->mac_len)) {
        printf("\nCrypto module error, HMAC_Final failed\n");
        return ACVP_CRYPTO_MODULE_FAIL;
    }
    HMAC_CTX_cleanup(&hmac_ctx);

    return ACVP_SUCCESS;
}

static ACVP_RESULT app_cmac_handler(ACVP_TEST_CASE *test_case)
{
    ACVP_CMAC_TC	*tc;
    const EVP_CIPHER	*c;
    CMAC_CTX       *cmac_ctx;
    int msg_len;

    if (!test_case) {
        return ACVP_INVALID_ARG;
    }

    tc = test_case->tc.cmac;

    switch (tc->cipher) {
    case ACVP_CMAC_AES_128:
      c = EVP_aes_128_cbc();
      break;
    case ACVP_CMAC_AES_192:
      c = EVP_aes_192_cbc();
      break;
    case ACVP_CMAC_AES_256:
      c = EVP_aes_256_cbc();
      break;
    case ACVP_CMAC_TDES:
      c = EVP_des_ede3_cbc();
      break;
    default:
    	printf("Error: Unsupported hash algorithm requested by ACVP server\n");
    	return ACVP_NO_CAP;
    	break;
    }

    cmac_ctx = CMAC_CTX_new();
    msg_len = tc->msg_len;

    if (!CMAC_Init(cmac_ctx, tc->key, tc->key_len, c, NULL)) {
        printf("\nCrypto module error, HMAC_Init_ex failed\n");
        return ACVP_CRYPTO_MODULE_FAIL;
    }

    if (!CMAC_Update(cmac_ctx, tc->msg, msg_len)) {
        printf("\nCrypto module error, HMAC_Update failed\n");
        return ACVP_CRYPTO_MODULE_FAIL;
    }

    if (!CMAC_Final(cmac_ctx, tc->mac, (size_t *)&tc->mac_len)) {
        printf("\nCrypto module error, HMAC_Final failed\n");
        return ACVP_CRYPTO_MODULE_FAIL;
    }
    CMAC_CTX_cleanup(cmac_ctx);

    return ACVP_SUCCESS;
}

static ACVP_RESULT app_rsa_handler(ACVP_TEST_CASE *test_case)
{
    /*
     * custom crypto module handler
     * to be filled in
     */
    ACVP_RSA_TC	*tc;
    RSA       *rsa;
    unsigned int bitlen1, bitlen2, bitlen3, bitlen4, seed_len, keylen;
    BIGNUM *exponent;
    unsigned long m;
    unsigned char *seed = NULL;

    if (!test_case) {
        return ACVP_INVALID_ARG;
    }

    tc = test_case->tc.rsa;
    switch(tc->mode) {
    case ACVP_RSA_MODE_KEYGEN:
        switch (tc->keygen_tc->rand_pq) {
        case RSA_RAND_PQ_B34: // "provPC"
            rsa = RSA_new();
            if(tc->keygen_tc->info_gen_by_server) {
                exponent = tc->keygen_tc->keygen_attrs_tc->e;
                bitlen1 = tc->keygen_tc->keygen_attrs_tc->bitlen1;
                bitlen2 = tc->keygen_tc->keygen_attrs_tc->bitlen2;
                bitlen3 = tc->keygen_tc->keygen_attrs_tc->bitlen3;
                bitlen4 = tc->keygen_tc->keygen_attrs_tc->bitlen4;
                seed_len = tc->keygen_tc->keygen_attrs_tc->seed_len;
                seed = tc->keygen_tc->keygen_attrs_tc->seed;
            } else {
                exponent = BN_new();
                m = RSA_F4;
                seed_len = 28;
                seed = calloc(seed_len, sizeof(char));
                if (!BN_set_word(exponent, m)) {
                    printf("Bignum API fail\n");
                    return ACVP_CRYPTO_MODULE_FAIL;
                }
                if (RAND_bytes(seed, seed_len) <= 0) {
                    printf("RAND API fail\n");
                    return ACVP_CRYPTO_MODULE_FAIL;
                }
                bitlen1 = 160;
                bitlen2 = 272;
                bitlen3 = 240;
                bitlen4 = 184;
                keylen = 2048;
            }
            break;
        case RSA_RAND_PQ_B32: // "provRP"
        case RSA_RAND_PQ_B33: // "probRP"
        case RSA_RAND_PQ_B35: // "bothPC"
        case RSA_RAND_PQ_B36: // "probPC"
        default:
            break;
        }
        break;

        if(rsa_generate_key_internal(&rsa->p, &rsa->q, &rsa->n, &rsa->d,
                                      seed, seed_len,
                                      bitlen1, bitlen2, bitlen3, bitlen4,
                                      exponent, keylen, NULL) != 1) {
            return ACVP_CRYPTO_MODULE_FAIL;
        }
        tc->keygen_tc->keygen_attrs_tc->p = rsa->p;
        tc->keygen_tc->keygen_attrs_tc->q = rsa->q;
        tc->keygen_tc->keygen_attrs_tc->n = rsa->n;
        tc->keygen_tc->keygen_attrs_tc->d = rsa->d;

        BN_free(exponent);
        RSA_free(rsa);
        break;
    default:
        break;
    }
    return ACVP_SUCCESS;
}

/**
 * copied from ciscossl-fom fips_utl.h
 */
unsigned char *hex2bin_m(const char *in, long *plen)
{
	unsigned char *p;
	if (strlen(in) == 0)
		{
		*plen = 0;
		return OPENSSL_malloc(1);
		}
	p = OPENSSL_malloc((strlen(in) + 1)/2);
	*plen = hex2bin(in, p);
	return p;
}

int hex2bin(const char *in, unsigned char *out)
{
    int n1, n2, isodd = 0;
    unsigned char ch;

    n1 = strlen(in);
    if (in[n1 - 1] == '\n')
	n1--;

    if (n1 & 1)
	isodd = 1;

    for (n1=0,n2=0 ; in[n1] && in[n1] != '\n' ; )
	{ /* first byte */
	if ((in[n1] >= '0') && (in[n1] <= '9'))
	    ch = in[n1++] - '0';
	else if ((in[n1] >= 'A') && (in[n1] <= 'F'))
	    ch = in[n1++] - 'A' + 10;
	else if ((in[n1] >= 'a') && (in[n1] <= 'f'))
	    ch = in[n1++] - 'a' + 10;
	else
	    return -1;
	if(!in[n1])
	    {
	    out[n2++]=ch;
	    break;
	    }
	/* If input is odd length first digit is least significant: assumes
	 * all digits valid hex and null terminated which is true for the
	 * strings we pass.
	 */
	if (n1 == 1 && isodd)
		{
		out[n2++] = ch;
		continue;
		}
	out[n2] = ch << 4;
	/* second byte */
	if ((in[n1] >= '0') && (in[n1] <= '9'))
	    ch = in[n1++] - '0';
	else if ((in[n1] >= 'A') && (in[n1] <= 'F'))
	    ch = in[n1++] - 'A' + 10;
	else if ((in[n1] >= 'a') && (in[n1] <= 'f'))
	    ch = in[n1++] - 'a' + 10;
	else
	    return -1;
	out[n2++] |= ch;
	}
    return n2;
}

/*
 * RSA SigGen handler
 */
static ACVP_RESULT app_rsa_siggen_handler(ACVP_TEST_CASE *test_case)
{
	RSA *rsa = NULL;
	EVP_MD *tc_md = NULL;
	unsigned char *msg = NULL;
	unsigned char *sigbuf = NULL;
	int siglen, pad_mode;
	long msglen = -1;
	BIGNUM *bn_e;

	ACVP_RSA_TC	*tc;

	if (!test_case) {
		return ACVP_INVALID_ARG;
	}

	tc = test_case->tc.rsa;

	if (!tc) {
		return ACVP_INVALID_ARG;
	}

    //	fips_algtest_init(); /*** necessito? ***/

	/*
	 * Set the message digest to the appropriate sha
	 */
	if(strncmp(tc->sig_tc->sig_attrs_tc->hash_alg, ACVP_RSA_SHA_1, RSA_HASH_ALG_MAX_LEN ) == 0 ) {
		tc_md = (EVP_MD *)EVP_sha1();
	} else if(strncmp(tc->sig_tc->sig_attrs_tc->hash_alg, ACVP_RSA_SHA_224, RSA_HASH_ALG_MAX_LEN ) == 0 ) {
		tc_md = (EVP_MD *)EVP_sha224();
	} else if(strncmp(tc->sig_tc->sig_attrs_tc->hash_alg, ACVP_RSA_SHA_256, RSA_HASH_ALG_MAX_LEN ) == 0 ) {
		tc_md = (EVP_MD *)EVP_sha256();
	} else if(strncmp(tc->sig_tc->sig_attrs_tc->hash_alg, ACVP_RSA_SHA_384, RSA_HASH_ALG_MAX_LEN ) == 0 ) {
		tc_md = (EVP_MD *)EVP_sha384();
	} else if(strncmp(tc->sig_tc->sig_attrs_tc->hash_alg, ACVP_RSA_SHA_512, RSA_HASH_ALG_MAX_LEN ) == 0 ) {
		tc_md = (EVP_MD *)EVP_sha512();
//	} else if(strncmp(tc->sig_tc->sig_attrs_tc->hash_alg, ACVP_RSA_SHA_512_224, RSA_HASH_ALG_MAX_LEN ) == 0 ) {
//		tc_md = (EVP_MD *)EVP_sha512224(); /*** what is this function for this hash? ***/
//	} else if(strncmp(tc->sig_tc->sig_attrs_tc->hash_alg, ACVP_RSA_SHA_512_256, RSA_HASH_ALG_MAX_LEN ) == 0 ) {
//		tc_md = (EVP_MD *)EVP_sha512256(); /*** what is this function for this hash? ***/
	} else {
		printf("\nError: hashAlg not supported for RSA SigGen\n");
		return ACVP_INVALID_ARG;
	}

	/*
	 * Set the message given from the tc to binary form
	 */
	msg = hex2bin_m(tc->sig_tc->sig_attrs_tc->msg, &msglen);
	if (msglen == -1) {
		printf("\nError: hex2bin error for RSA SigGen\n");
		return ACVP_INVALID_ARG;
	}

	/*
	 * Make an RSA object and set a new BN exponent to use to generate a key
	 */
	if (rsa) {
		FIPS_rsa_free(rsa);
	}

	rsa = FIPS_rsa_new();
	if (!rsa) {
		printf("\nError: Issue with RSA obj in RSA SigGen\n");
		return ACVP_CRYPTO_MODULE_FAIL;
	}

	bn_e = BN_new();
	if (!bn_e || !BN_set_word(bn_e, 0x1001)) {
		printf("\nError: Issue with exponent in RSA SigGen\n");
		return ACVP_CRYPTO_MODULE_FAIL;
	}

	if (!tc->sig_tc->sig_attrs_tc->modulo) {
		printf("\nError: Issue with modulo in RSA SigGen\n");
		return ACVP_CRYPTO_MODULE_FAIL;
	}

	/*
	 * Set the pad mode and generate a key given the respective sigType
	 */
	if(strncmp(tc->sig_tc->sig_type, RSA_SIG_TYPE_X931_NAME, RSA_SIG_TYPE_MAX ) == 0 ) {
		pad_mode = RSA_X931_PADDING;
		if (!RSA_X931_generate_key_ex(rsa, tc->sig_tc->sig_attrs_tc->modulo, bn_e, NULL)) {
			printf("\nError: Issue with keygen during siggen mode for sigType %s\n",RSA_SIG_TYPE_X931_NAME);
			return ACVP_CRYPTO_MODULE_FAIL;
		}
	} else if(strncmp(tc->sig_tc->sig_type, RSA_SIG_TYPE_PKCS1V15_NAME, RSA_SIG_TYPE_MAX ) == 0 ) {
		pad_mode = RSA_PKCS1_PADDING;
		if (!RSA_X931_generate_key_ex(rsa, tc->sig_tc->sig_attrs_tc->modulo, bn_e, NULL)) { /*** only need to (can) generate key with x931-- sigType doesn't matter for key generation ***/
			printf("\nError: Issue with keygen during siggen mode for sigType PKCS1V15\n");
			return ACVP_CRYPTO_MODULE_FAIL;
		}
	} else if(strncmp(tc->sig_tc->sig_type, RSA_SIG_TYPE_PKCS1PSS_NAME, RSA_SIG_TYPE_MAX ) == 0 ) {
		pad_mode = RSA_PKCS1_PSS_PADDING;
		if (!RSA_X931_generate_key_ex(rsa, tc->sig_tc->sig_attrs_tc->modulo, bn_e, NULL)) { /*** only need to (can) generate key with x931-- sigType doesn't matter for key generation ***/
			printf("\nError: Issue with keygen during siggen mode for sigType PKCS1PSS\n");
			return ACVP_CRYPTO_MODULE_FAIL;
		}
	} else {
		printf("\nError: sigType not supported\n");
		return ACVP_INVALID_ARG;
	}

	BN_free(bn_e);


	/*
	 * Retrieve and save the exponent and modulus from the key generation process
	 */
	tc->sig_tc->sig_attrs_tc->e = rsa->e;
	tc->sig_tc->sig_attrs_tc->n = rsa->n;

	if (msg && tc_md) {
		siglen = RSA_size(rsa);
		sigbuf = OPENSSL_malloc(siglen);

		if (!sigbuf) {
			printf("\nError: SigBuf fail in RSA SigGen\n");
			return ACVP_CRYPTO_MODULE_FAIL;
		}

		if (!FIPS_rsa_sign(rsa, msg, msglen, tc_md, pad_mode, tc->sig_tc->sig_attrs_tc->salt_len, NULL,
								sigbuf, (unsigned int *)&siglen)) {
			printf("\nError: RSA Signature Generation fail\n");
			return ACVP_CRYPTO_MODULE_FAIL;
		}

		/*
		 * Retrieve and save the signature generated from signing the generated key
		 */
		tc->sig_tc->sig_attrs_tc->s = BN_bin2bn(sigbuf, siglen, calloc(1,sizeof(BIGNUM)));
		if (sigbuf)
			OPENSSL_free(sigbuf);
	}
	return ACVP_SUCCESS;
}

/* RSA SigVer handler
 * msg and sig are binary format
 * exponent and modulus are hex values
 */
static ACVP_RESULT app_rsa_sigver_handler(const unsigned char* msg, const int msg_len, const unsigned char* sig,
                                          const int sig_len, const char* exponent, const char* modulus,
                                          int rsa_sig_type, int hash)
{
    int ret = -1;
    int padding;
    BIGNUM *tc_modulus = NULL;
    BIGNUM *tc_exponent = NULL;
    EVP_MD_CTX *tc_mdctx = NULL;
    EVP_MD *tc_md = NULL;
    BN_hex2bn(&tc_exponent, exponent);
    BN_hex2bn(&tc_modulus, modulus);

    RSA *rsa_key = NULL;
    EVP_PKEY *rsa_evpkey = NULL;
    EVP_PKEY_CTX *rsa_pkctx = NULL;


    //TODO replace numerics when defined
    switch(rsa_sig_type){
    case 0:  //    RSA_SIG_TYPE_X931 = 0,
        padding = RSA_X931_PADDING;
        break;
    case 1:  //    RSA_SIG_TYPE_PKCS1V15
        padding = RSA_PKCS1_PADDING;
        break;
    case 2: //RSA_SIG_TYPE_PKCS1PSS
        padding = RSA_PKCS1_PSS_PADDING;
        break;
    default:
        printf( "ERROR: Unsupported RSA padding.\n");
        goto err;
    }

    switch (hash) {
    case ACVP_SHA1:
        tc_md = (EVP_MD *)EVP_sha1();
        break;
    case ACVP_SHA224:
        tc_md = (EVP_MD *)EVP_sha224();
        break;
    case ACVP_SHA256:
        tc_md = (EVP_MD *)EVP_sha256();
        break;
    case ACVP_SHA384:
        tc_md = (EVP_MD *)EVP_sha384();
        break;
    case ACVP_SHA512:
        tc_md = (EVP_MD *)EVP_sha512();
        break;
    default:
        printf( "ERROR: Unsupported hash.\n");
        goto err;
    }

    /* Create RSA Public key from input */
    rsa_key = RSA_new();
    if (rsa_key) {
        rsa_key->n    = BN_dup(tc_modulus);
        rsa_key->e    = BN_dup(tc_exponent);
        rsa_key->iqmp = NULL;
        rsa_key->d    = NULL;
        rsa_key->p    = NULL;
        rsa_key->q    = NULL;

        rsa_evpkey = EVP_PKEY_new();
        if (!rsa_evpkey) {
            printf( "ERROR: EVP_PKEY_new() failed\n");
            goto err;
        }
        EVP_PKEY_assign_RSA(rsa_evpkey, rsa_key);


        /* Create the Message Digest Context */
        if (!(tc_mdctx = EVP_MD_CTX_create())) {
            printf( "ERROR: EVP_MD_CTX_create() failed\n");
            goto err;
        }

        rsa_pkctx = EVP_PKEY_CTX_new(rsa_evpkey, NULL);
        if (!rsa_pkctx) {
            printf( "ERROR: EVP_PKEY_CTX_new() failed\n");
            goto err;
        }

        long lerr;
        /* Initialize operation  */
        ret = EVP_DigestVerifyInit(tc_mdctx, &rsa_pkctx, tc_md, NULL, rsa_evpkey);
        if (ret != 1) {
            printf("ERROR: EVP_DigestVerifyInit failed.\n");
            while ((lerr = ERR_get_error()))
                printf( "ERROR:%s\n", ERR_error_string(lerr, NULL));
            goto err;
        }

        /* set padding */
        ret = EVP_PKEY_CTX_set_rsa_padding(rsa_pkctx, padding);
        if (ret != 1) {
            printf("ERROR: EVP_PKEY_CTX_set_rsa_padding failed.\n");
            while ((lerr = ERR_get_error()))
                printf( "ERROR:%s\n", ERR_error_string(lerr, NULL));
            goto err;
        }

        /* update the message */
        ret = EVP_DigestVerifyUpdate(tc_mdctx, msg, msg_len);
        if (ret != 1) {
            printf("ERROR: EVP_DigestVerifyUpdate failed.\n");
            while ((lerr = ERR_get_error()))
                printf( "ERROR:%s\n", ERR_error_string(lerr, NULL));
            goto err;
        }

        /* Finalize */
        ret = EVP_DigestVerifyFinal(tc_mdctx, sig, sig_len);
        if (ret != 1) {
            printf("ERROR: RSA Digest verify failed.\n");
            while ((lerr = ERR_get_error()))
                printf( "ERROR:%s\n", ERR_error_string(lerr, NULL));
            goto err;
        }
    }

err:
    if (rsa_key) RSA_free(rsa_key);
    if (rsa_evpkey) EVP_PKEY_free(rsa_evpkey);
    if (tc_mdctx) EVP_MD_CTX_destroy(tc_mdctx);
    if (rsa_pkctx) EVP_PKEY_CTX_free(rsa_pkctx);
    if (tc_modulus) BN_free(tc_modulus);
    if (tc_exponent) BN_free(tc_exponent);

    if (ret == 1)
        return ACVP_SUCCESS;
    else
        return ACVP_UNSUPPORTED_OP;
}


#ifdef ACVP_NO_RUNTIME
typedef struct
{
    unsigned char *ent;
    size_t entlen;
    unsigned char *nonce;
    size_t noncelen;
} DRBG_TEST_ENT;

static size_t drbg_test_entropy(DRBG_CTX *dctx, unsigned char **pout,
        int entropy, size_t min_len, size_t max_len)
{
    if (!dctx || !pout) return 0;
    DRBG_TEST_ENT *t = (DRBG_TEST_ENT *)FIPS_drbg_get_app_data(dctx);
    if (!t) return 0;

    if (t->entlen < min_len) printf("entropy data len < min_len: %zu\n", t->entlen);
    if (t->entlen > max_len) printf("entropy data len > max_len: %zu\n", t->entlen);
    *pout = (unsigned char *)t->ent;
    return t->entlen;
}

static size_t drbg_test_nonce(DRBG_CTX *dctx, unsigned char **pout,
        int entropy, size_t min_len, size_t max_len)
{
    if (!dctx || !pout) return 0;
    DRBG_TEST_ENT *t = (DRBG_TEST_ENT *)FIPS_drbg_get_app_data(dctx);

    if (t->noncelen < min_len) printf("nonce data len < min_len: %zu\n", t->noncelen);
    if (t->noncelen > max_len) printf("nonce data len > max_len: %zu\n", t->noncelen);
    *pout = (unsigned char *)t->nonce;
    return t->noncelen;
}

static ACVP_RESULT app_drbg_handler(ACVP_TEST_CASE *test_case)
{
    ACVP_RESULT     result = ACVP_SUCCESS;
    ACVP_DRBG_TC    *tc;
    unsigned int    nid;
    int             der_func = 0;
    unsigned int    drbg_entropy_len;
    int             fips_rc;

    unsigned char   *nonce = NULL;

    if (!test_case) {
        return ACVP_INVALID_ARG;
    }

    tc = test_case->tc.drbg;
    /*
     * Init entropy length
     */
    drbg_entropy_len = tc->entropy_len;

    switch(tc->cipher) {
    case ACVP_HASHDRBG:
        nonce = tc->nonce;
        switch(tc->mode) {
        case ACVP_DRBG_SHA_1:
            nid = NID_sha1;
            break;
        case ACVP_DRBG_SHA_224:
            nid = NID_sha256;
            break;
        case ACVP_DRBG_SHA_256:
            nid = NID_sha256;
            break;
        case ACVP_DRBG_SHA_384:
            nid = NID_sha384;
            break;
        case ACVP_DRBG_SHA_512:
            nid = NID_sha512;
            break;

        case ACVP_DRBG_SHA_512_224:
        case ACVP_DRBG_SHA_512_256:
        default:
            result = ACVP_UNSUPPORTED_OP;
            printf("%s: Unsupported algorithm/mode %d/%d (tc_id=%d)\n", __FUNCTION__, tc->tc_id,
                    tc->cipher, tc->mode);
            return (result);
            break;
    }
    break;

        case ACVP_HMACDRBG:
            nonce = tc->nonce;
            switch(tc->mode) {
            case ACVP_DRBG_SHA_1:
                nid =   NID_hmacWithSHA1;
                break;
            case ACVP_DRBG_SHA_224:
                nid =   NID_hmacWithSHA224;
                break;
            case ACVP_DRBG_SHA_256:
                nid =   NID_hmacWithSHA256;
                break;
            case ACVP_DRBG_SHA_384:
                nid =   NID_hmacWithSHA384;
                break;
            case ACVP_DRBG_SHA_512:
                nid =   NID_hmacWithSHA512;
                break;
            case ACVP_DRBG_SHA_512_224:
            case ACVP_DRBG_SHA_512_256:
            default:
                result = ACVP_UNSUPPORTED_OP;
                printf("%s: Unsupported algorithm/mode %d/%d (tc_id=%d)\n", __FUNCTION__, tc->tc_id,
                        tc->cipher, tc->mode);
                return (result);
                break;
            }
        break;

        case ACVP_CTRDRBG:
            /*
             * DR function Only valid in CTR mode
             * if not set nonce is ignored
             */
            if (tc->der_func_enabled) {
                der_func = DRBG_FLAG_CTR_USE_DF;
                nonce = tc->nonce;
            } else {
                /**
                 * Note 5: All DRBGs are tested at their maximum supported security
                 * strength so this is the minimum bit length of the entropy input that
                 * ACVP will accept.  The maximum supported security strength is also
                 * the default value for this input.  Longer entropy inputs are
                 * permitted, with the following exception: for ctrDRBG with no df, the
                 * bit length must equal the seed length.
                 **/
                drbg_entropy_len = 0;
            }

            switch(tc->mode) {
            case ACVP_DRBG_AES_128:
                nid = NID_aes_128_ctr;
                break;
            case ACVP_DRBG_AES_192:
                nid = NID_aes_192_ctr;
                break;
            case ACVP_DRBG_AES_256:
                nid = NID_aes_256_ctr;
                break;
            case ACVP_DRBG_3KEYTDEA:
            default:
                result = ACVP_UNSUPPORTED_OP;
                printf("%s: Unsupported algorithm/mode %d/%d (tc_id=%d)\n", __FUNCTION__, tc->tc_id,
                        tc->cipher, tc->mode);
                return (result);
                break;
            }
        break;
        default:
            result = ACVP_UNSUPPORTED_OP;
            printf("%s: Unsupported algorithm %d (tc_id=%d)\n", __FUNCTION__, tc->tc_id,
                    tc->cipher);
            return (result);
            break;
    }

    DRBG_CTX *drbg_ctx = NULL;
    DRBG_TEST_ENT entropy_nonce;
    memset(&entropy_nonce, 0, sizeof(DRBG_TEST_ENT));
    drbg_ctx = FIPS_drbg_new(nid, der_func | DRBG_FLAG_TEST);
    if (!drbg_ctx) {
        progress("ERROR: failed to create DRBG Context.");
        return ACVP_MALLOC_FAIL;
    }

    /*
     * Set entropy and nonce
     */
    entropy_nonce.ent = tc->entropy;
    entropy_nonce.entlen = tc->entropy_len/8;

    entropy_nonce.nonce = nonce;
    entropy_nonce.noncelen = tc->nonce_len/8;

    FIPS_drbg_set_app_data(drbg_ctx, &entropy_nonce);

    fips_rc = FIPS_drbg_set_callbacks(drbg_ctx,
                                      drbg_test_entropy,
                                      0, 0,
                                      drbg_test_nonce,
                                      0);
    if (!fips_rc) {
        progress("ERROR: failed to Set callback DRBG ctx");
        long l = 9;
        char buf[2048]  = {0};
        while ((l = ERR_get_error()))
            printf( "ERROR:%s\n", ERR_error_string(l, buf));

        result = ACVP_CRYPTO_MODULE_FAIL;
        goto end;
    }

    fips_rc = FIPS_drbg_instantiate(drbg_ctx, (const unsigned char *)tc->perso_string,
                                    (size_t) tc->perso_string_len/8);
    if (!fips_rc) {
        progress("ERROR: failed to instantiate DRBG ctx");
        long l = 9;
        char buf[2048]  = {0};
        while ((l = ERR_get_error()))
            printf( "ERROR:%s\n", ERR_error_string(l, buf));

        result = ACVP_CRYPTO_MODULE_FAIL;
        goto end;
    }

    /*
     * Process predictive resistance flag
     */
    if (tc->pred_resist_enabled) {
        entropy_nonce.ent = tc->entropy_input_pr;
        entropy_nonce.entlen = tc->entropy_len/8;

        fips_rc =  FIPS_drbg_generate(drbg_ctx, (unsigned char *)tc->drb,
                                  (size_t) (tc->drb_len/8),
                                  (int) 1,
                                  (const unsigned char *)tc->additional_input,
                                  (size_t) (tc->additional_input_len/8));
        if (!fips_rc) {
            progress("ERROR: failed to generate drb");
            long l;
            while ((l = ERR_get_error()))
                printf( "ERROR:%s\n", ERR_error_string(l, NULL));
            result = ACVP_CRYPTO_MODULE_FAIL;
            goto end;
        }

        entropy_nonce.ent = tc->entropy_input_pr_1;
        entropy_nonce.entlen = tc->entropy_len/8;

        fips_rc =  FIPS_drbg_generate(drbg_ctx, (unsigned char *)tc->drb,
                                  (size_t) (tc->drb_len/8),
                                  (int) 1,
                                  (const unsigned char *)tc->additional_input_1,
                                  (size_t) (tc->additional_input_len/8));
        if (!fips_rc) {
            progress("ERROR: failed to generate drb");
            long l;
            while ((l = ERR_get_error()))
                printf( "ERROR:%s\n", ERR_error_string(l, NULL));
            result = ACVP_CRYPTO_MODULE_FAIL;
            goto end;
        }
    } else {
        fips_rc = FIPS_drbg_generate(drbg_ctx, (unsigned char *)tc->drb,
                                     (size_t) (tc->drb_len/8),
                                     (int) 0,
                                     (const unsigned char *)tc->additional_input,
                                     (size_t) (tc->additional_input_len/8));
        if (!fips_rc) {
            progress("ERROR: failed to generate drb");
            long l;
            while ((l = ERR_get_error()))
                printf( "ERROR:%s\n", ERR_error_string(l, NULL));
            result = ACVP_CRYPTO_MODULE_FAIL;
            goto end;
        }
    }

end:
    FIPS_drbg_uninstantiate(drbg_ctx);
    FIPS_drbg_free(drbg_ctx);

    return result;
}
#endif
