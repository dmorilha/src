/*
 * Copyright (c) 2021 Yubico AB. All rights reserved.
 * Use of this source code is governed by a BSD-style
 * license that can be found in the LICENSE file.
 */

#include <openssl/rsa.h>
#include <openssl/obj_mac.h>

#include "fido.h"

#if OPENSSL_VERSION_NUMBER >= 0x30000000
static EVP_MD *
rs1_get_EVP_MD(void)
{
	return (EVP_MD_fetch(NULL, "SHA-1", NULL));
}

static void
rs1_free_EVP_MD(EVP_MD *md)
{
	EVP_MD_free(md);
}
#else
static EVP_MD *
rs1_get_EVP_MD(void)
{
	return ((EVP_MD *)EVP_sha1());
}

static void
rs1_free_EVP_MD(EVP_MD *md)
{
	(void)md;
}
#endif /* OPENSSL_VERSION_NUMBER */

int
rs1_verify_sig(const fido_blob_t *dgst, EVP_PKEY *pkey,
    const fido_blob_t *sig)
{
	EVP_PKEY_CTX	*pctx = NULL;
	EVP_MD		*md = NULL;
	int		 ok = -1;

	if (EVP_PKEY_base_id(pkey) != EVP_PKEY_RSA) {
		fido_log_debug("%s: EVP_PKEY_base_id", __func__);
		goto fail;
	}

	if ((md = rs1_get_EVP_MD()) == NULL) {
		fido_log_debug("%s: rs1_get_EVP_MD", __func__);
		goto fail;
	}

	if ((pctx = EVP_PKEY_CTX_new(pkey, NULL)) == NULL ||
	    EVP_PKEY_verify_init(pctx) != 1 ||
	    EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PADDING) != 1 ||
	    EVP_PKEY_CTX_set_signature_md(pctx, md) != 1) {
		fido_log_debug("%s: EVP_PKEY_CTX", __func__);
		goto fail;
	}

	if (EVP_PKEY_verify(pctx, sig->ptr, sig->len, dgst->ptr,
	    dgst->len) != 1) {
		fido_log_debug("%s: EVP_PKEY_verify", __func__);
		goto fail;
	}

	ok = 0;
fail:
	EVP_PKEY_CTX_free(pctx);
	rs1_free_EVP_MD(md);

	return (ok);
}
