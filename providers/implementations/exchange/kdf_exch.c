/*
 * Copyright 2020 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include <openssl/crypto.h>
#include <openssl/kdf.h>
#include <openssl/core_dispatch.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
#include "prov/implementations.h"
#include "prov/provider_ctx.h"
#include "prov/kdfexchange.h"

static OSSL_FUNC_keyexch_newctx_fn kdf_tls1_prf_newctx;
static OSSL_FUNC_keyexch_newctx_fn kdf_hkdf_newctx;
static OSSL_FUNC_keyexch_newctx_fn kdf_scrypt_newctx;
static OSSL_FUNC_keyexch_init_fn kdf_init;
static OSSL_FUNC_keyexch_derive_fn kdf_derive;
static OSSL_FUNC_keyexch_freectx_fn kdf_freectx;
static OSSL_FUNC_keyexch_dupctx_fn kdf_dupctx;
static OSSL_FUNC_keyexch_set_ctx_params_fn kdf_set_ctx_params;
static OSSL_FUNC_keyexch_settable_ctx_params_fn kdf_tls1_prf_settable_ctx_params;
static OSSL_FUNC_keyexch_settable_ctx_params_fn kdf_hkdf_settable_ctx_params;
static OSSL_FUNC_keyexch_settable_ctx_params_fn kdf_scrypt_settable_ctx_params;

typedef struct {
    void *provctx;
    EVP_KDF_CTX *kdfctx;
    KDF_DATA *kdfdata;
} PROV_KDF_CTX;

#define KDF_NEWCTX(funcname, kdfname) \
    static void *kdf_##funcname##_newctx(void *provctx) \
    { \
        PROV_KDF_CTX *kdfctx = OPENSSL_zalloc(sizeof(PROV_KDF_CTX)); \
        EVP_KDF *kdf = NULL; \
        \
        if (kdfctx == NULL) \
            return NULL; \
        \
        kdfctx->provctx = provctx; \
        \
        kdf = EVP_KDF_fetch(PROV_LIBRARY_CONTEXT_OF(provctx), kdfname, NULL); \
        if (kdf == NULL) \
            goto err; \
        kdfctx->kdfctx = EVP_KDF_CTX_new(kdf); \
        EVP_KDF_free(kdf); \
        \
        if (kdfctx->kdfctx == NULL) \
            goto err; \
        \
        return kdfctx; \
    err: \
        OPENSSL_free(kdfctx); \
        return NULL; \
    }

KDF_NEWCTX(tls1_prf, "TLS1-PRF")
KDF_NEWCTX(hkdf, "HKDF")
KDF_NEWCTX(scrypt, "SCRYPT")

static int kdf_init(void *vpkdfctx, void *vkdf)
{
    PROV_KDF_CTX *pkdfctx = (PROV_KDF_CTX *)vpkdfctx;

    if (pkdfctx == NULL || vkdf == NULL || !kdf_data_up_ref(vkdf))
        return 0;
    pkdfctx->kdfdata = vkdf;

    return 1;
}

static int kdf_derive(void *vpkdfctx, unsigned char *secret, size_t *secretlen,
                      size_t outlen)
{
    PROV_KDF_CTX *pkdfctx = (PROV_KDF_CTX *)vpkdfctx;

    return EVP_KDF_derive(pkdfctx->kdfctx, secret, *secretlen);
}

static void kdf_freectx(void *vpkdfctx)
{
    PROV_KDF_CTX *pkdfctx = (PROV_KDF_CTX *)vpkdfctx;

    EVP_KDF_CTX_free(pkdfctx->kdfctx);
    kdf_data_free(pkdfctx->kdfdata);

    OPENSSL_free(pkdfctx);
}

static void *kdf_dupctx(void *vpkdfctx)
{
    PROV_KDF_CTX *srcctx = (PROV_KDF_CTX *)vpkdfctx;
    PROV_KDF_CTX *dstctx;

    dstctx = OPENSSL_zalloc(sizeof(*srcctx));
    if (dstctx == NULL)
        return NULL;

    *dstctx = *srcctx;

    dstctx->kdfctx = EVP_KDF_CTX_dup(srcctx->kdfctx);
    if (dstctx->kdfctx == NULL) {
        OPENSSL_free(dstctx);
        return NULL;
    }
    if (!kdf_data_up_ref(dstctx->kdfdata)) {
        EVP_KDF_CTX_free(dstctx->kdfctx);
        OPENSSL_free(dstctx);
        return NULL;
    }

    return dstctx;
}

static int kdf_set_ctx_params(void *vpkdfctx, const OSSL_PARAM params[])
{
    PROV_KDF_CTX *pkdfctx = (PROV_KDF_CTX *)vpkdfctx;

    return EVP_KDF_CTX_set_params(pkdfctx->kdfctx, params);
}

#define KDF_SETTABLE_CTX_PARAMS(funcname, kdfname) \
    static const OSSL_PARAM *kdf_##funcname##_settable_ctx_params(void) \
    { \
        /* \
        * TODO(3.0): FIXME FIXME!! These settable_ctx_params functions should \
        * should have a provctx argument so we can get hold of the libctx. \
        */ \
        EVP_KDF *kdf = EVP_KDF_fetch(NULL, kdfname, NULL); \
        const OSSL_PARAM *params; \
        \
        if (kdf == NULL) \
            return NULL; \
        \
        params = EVP_KDF_settable_ctx_params(kdf); \
        EVP_KDF_free(kdf); \
        \
        return params; \
    }

KDF_SETTABLE_CTX_PARAMS(tls1_prf, "TLS1-PRF")
KDF_SETTABLE_CTX_PARAMS(hkdf, "HKDF")
KDF_SETTABLE_CTX_PARAMS(scrypt, "SCRYPT")


#define KDF_KEYEXCH_FUNCTIONS(funcname) \
    const OSSL_DISPATCH kdf_##funcname##_keyexch_functions[] = { \
        { OSSL_FUNC_KEYEXCH_NEWCTX, (void (*)(void))kdf_##funcname##_newctx }, \
        { OSSL_FUNC_KEYEXCH_INIT, (void (*)(void))kdf_init }, \
        { OSSL_FUNC_KEYEXCH_DERIVE, (void (*)(void))kdf_derive }, \
        { OSSL_FUNC_KEYEXCH_FREECTX, (void (*)(void))kdf_freectx }, \
        { OSSL_FUNC_KEYEXCH_DUPCTX, (void (*)(void))kdf_dupctx }, \
        { OSSL_FUNC_KEYEXCH_SET_CTX_PARAMS, (void (*)(void))kdf_set_ctx_params }, \
        { OSSL_FUNC_KEYEXCH_SETTABLE_CTX_PARAMS, \
        (void (*)(void))kdf_##funcname##_settable_ctx_params }, \
        { 0, NULL } \
    };

KDF_KEYEXCH_FUNCTIONS(tls1_prf)
KDF_KEYEXCH_FUNCTIONS(hkdf)
KDF_KEYEXCH_FUNCTIONS(scrypt)