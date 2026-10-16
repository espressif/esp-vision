/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <openssl/evp.h>
#include <stddef.h>
typedef struct {
    EVP_MD_CTX *context;
} psa_hash_operation_t;
#define PSA_HASH_OPERATION_INIT { NULL }
#define PSA_SUCCESS 0
#define PSA_ALG_SHA_256 1
static inline int psa_crypto_init(void)
{
    return 0;
}
static inline int psa_hash_setup(psa_hash_operation_t *op, int algorithm)
{
    (void)algorithm;
    op->context = EVP_MD_CTX_new();
    return op->context && EVP_DigestInit_ex(op->context, EVP_sha256(), NULL) ? 0 : -1;
}
static inline int psa_hash_update(psa_hash_operation_t *op, const unsigned char *bytes, size_t size)
{
    return EVP_DigestUpdate(op->context, bytes, size) ? 0 : -1;
}
static inline int psa_hash_finish(psa_hash_operation_t *op, unsigned char *bytes, size_t capacity, size_t *size)
{
    if (capacity < 32) {
        return -1;
    } unsigned int count;
    int ok = EVP_DigestFinal_ex(op->context, bytes, &count);
    EVP_MD_CTX_free(op->context);
    op->context = NULL;
    *size = count;
    return ok ? 0 : -1;
}
static inline int psa_hash_abort(psa_hash_operation_t *op)
{
    EVP_MD_CTX_free(op->context);
    op->context = NULL;
    return 0;
}
