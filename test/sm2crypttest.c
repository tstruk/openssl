/*
 * Copyright 2017 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the OpenSSL license (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include "testutil.h"

#ifndef OPENSSL_NO_SM2

# include <openssl/sm2.h>

static RAND_METHOD fake_rand;
static const RAND_METHOD *saved_rand;

static uint8_t *fake_rand_bytes = NULL;
static size_t fake_rand_bytes_offset = 0;

static int get_faked_bytes(unsigned char *buf, int num)
{
    int i;

    if (fake_rand_bytes == NULL)
        return saved_rand->bytes(buf, num);

    for (i = 0; i != num; ++i)
        buf[i] = fake_rand_bytes[fake_rand_bytes_offset + i];
    fake_rand_bytes_offset += num;
    return 1;
}

static int start_fake_rand(const char *hex_bytes)
{
    /* save old rand method */
    if (!TEST_ptr(saved_rand = RAND_get_rand_method()))
        return 0;

    fake_rand = *saved_rand;
    /* use own random function */
    fake_rand.bytes = get_faked_bytes;

    fake_rand_bytes = OPENSSL_hexstr2buf(hex_bytes, NULL);
    fake_rand_bytes_offset = 0;

    /* set new RAND_METHOD */
    if (!TEST_true(RAND_set_rand_method(&fake_rand)))
        return 0;
    return 1;
}

static int restore_rand(void)
{
    OPENSSL_free(fake_rand_bytes);
    fake_rand_bytes = NULL;
    fake_rand_bytes_offset = 0;
    if (!TEST_true(RAND_set_rand_method(saved_rand)))
        return 0;
    return 1;
}

static EC_GROUP *create_EC_group(const char *p_hex, const char *a_hex,
                                 const char *b_hex, const char *x_hex,
                                 const char *y_hex, const char *order_hex,
                                 const char *cof_hex)
{
    BIGNUM *p = NULL;
    BIGNUM *a = NULL;
    BIGNUM *b = NULL;
    BIGNUM *g_x = NULL;
    BIGNUM *g_y = NULL;
    BIGNUM *order = NULL;
    BIGNUM *cof = NULL;
    EC_POINT *generator = NULL;
    EC_GROUP *group = NULL;

    BN_hex2bn(&p, p_hex);
    BN_hex2bn(&a, a_hex);
    BN_hex2bn(&b, b_hex);

    group = EC_GROUP_new_curve_GFp(p, a, b, NULL);
    BN_free(p);
    BN_free(a);
    BN_free(b);

    if (group == NULL)
        return NULL;

    generator = EC_POINT_new(group);
    if (generator == NULL)
        return NULL;

    BN_hex2bn(&g_x, x_hex);
    BN_hex2bn(&g_y, y_hex);

    if (EC_POINT_set_affine_coordinates_GFp(group, generator, g_x, g_y, NULL) ==
        0)
        return NULL;

    BN_free(g_x);
    BN_free(g_y);

    BN_hex2bn(&order, order_hex);
    BN_hex2bn(&cof, cof_hex);

    if (EC_GROUP_set_generator(group, generator, order, cof) == 0)
        return NULL;

    EC_POINT_free(generator);
    BN_free(order);
    BN_free(cof);

    return group;
}

static int test_sm2(const EC_GROUP *group,
                    const EVP_MD *digest,
                    const char *privkey_hex,
                    const char *message,
                    const char *k_hex, const char *ctext_hex)
{
    const size_t msg_len = strlen(message);

    BIGNUM *priv = NULL;
    EC_KEY *key = NULL;
    EC_POINT *pt = NULL;
    unsigned char *expected = OPENSSL_hexstr2buf(ctext_hex, NULL);

    size_t ctext_len = 0;
    size_t ptext_len = 0;
    uint8_t *ctext = NULL;
    uint8_t *recovered = NULL;
    size_t recovered_len = msg_len;

    int rc = 0;

    BN_hex2bn(&priv, privkey_hex);

    key = EC_KEY_new();
    EC_KEY_set_group(key, group);
    EC_KEY_set_private_key(key, priv);

    pt = EC_POINT_new(group);
    EC_POINT_mul(group, pt, priv, NULL, NULL, NULL);

    EC_KEY_set_public_key(key, pt);
    BN_free(priv);
    EC_POINT_free(pt);

    ctext_len = SM2_ciphertext_size(key, digest, msg_len);
    ctext = OPENSSL_zalloc(ctext_len);
    if (ctext == NULL)
        goto done;

    start_fake_rand(k_hex);
    rc = SM2_encrypt(key, digest,
                     (const uint8_t *)message, msg_len, ctext, &ctext_len);
    restore_rand();

    TEST_mem_eq(ctext, ctext_len, expected, ctext_len);
    if (rc == 0)
        goto done;

    ptext_len = SM2_plaintext_size(key, digest, ctext_len);

    TEST_int_eq(ptext_len, msg_len);

    recovered = OPENSSL_zalloc(ptext_len);
    if (recovered == NULL)
        goto done;
    rc = SM2_decrypt(key, digest, ctext, ctext_len, recovered, &recovered_len);

    TEST_int_eq(recovered_len, msg_len);
    TEST_mem_eq(recovered, recovered_len, message, msg_len);
    if (rc == 0)
        return 0;

    rc = 1;
 done:

    OPENSSL_free(ctext);
    OPENSSL_free(recovered);
    OPENSSL_free(expected);
    EC_KEY_free(key);
    return rc;
}

static int sm2_crypt_test(void)
{
    int rc;
    EC_GROUP *test_group =
        create_EC_group
        ("8542D69E4C044F18E8B92435BF6FF7DE457283915C45517D722EDB8B08F1DFC3",
         "787968B4FA32C3FD2417842E73BBFEFF2F3C848B6831D7E0EC65228B3937E498",
         "63E4C6D3B23B0C849CF84241484BFE48F61D59A5B16BA06E6E12D1DA27C5249A",
         "421DEBD61B62EAB6746434EBC3CC315E32220B3BADD50BDC4C4E6C147FEDD43D",
         "0680512BCBB42C07D47349D2153B70C4E5D7FDFCBFA36EA1A85841B9E46E09A2",
         "8542D69E4C044F18E8B92435BF6FF7DD297720630485628D5AE74EE7C32E79B7",
         "1");

    if (test_group == NULL)
        return 0;

    rc = test_sm2(test_group,
                  EVP_sm3(),
                  "1649AB77A00637BD5E2EFE283FBF353534AA7F7CB89463F208DDBC2920BB0DA0",
                  "encryption standard",
                  "004C62EEFD6ECFC2B95B92FD6C3D9575148AFA17425546D49018E5388D49DD7B4F",
                  "307B0220245C26FB68B1DDDDB12C4B6BF9F2B6D5FE60A383B0D18D1C4144ABF1"
                  "7F6252E7022076CB9264C2A7E88E52B19903FDC47378F605E36811F5C07423A2"
                  "4B84400F01B804209C3D7360C30156FAB7C80A0276712DA9D8094A634B766D3A"
                  "285E07480653426D0413650053A89B41C418B0C3AAD00D886C00286467");

    if (rc == 0)
        return 0;

    /* Same test as above except using SHA-256 instead of SM3 */
    rc = test_sm2(test_group,
                  EVP_sha256(),
                  "1649AB77A00637BD5E2EFE283FBF353534AA7F7CB89463F208DDBC2920BB0DA0",
                  "encryption standard",
                  "004C62EEFD6ECFC2B95B92FD6C3D9575148AFA17425546D49018E5388D49DD7B4F",
                  "307B0220245C26FB68B1DDDDB12C4B6BF9F2B6D5FE60A383B0D18D1C4144ABF17F6252E7022076CB9264C2A7E88E52B19903FDC47378F605E36811F5C07423A24B84400F01B80420BE89139D07853100EFA763F60CBE30099EA3DF7F8F364F9D10A5E988E3C5AAFC0413229E6C9AEE2BB92CAD649FE2C035689785DA33");
    if (rc == 0)
        return 0;

    EC_GROUP_free(test_group);

    return 1;
}

#endif

int setup_tests(void)
{
#ifdef OPENSSL_NO_SM2
    TEST_note("SM2 is disabled.");
#else
    ADD_TEST(sm2_crypt_test);
#endif
    return 1;
}
