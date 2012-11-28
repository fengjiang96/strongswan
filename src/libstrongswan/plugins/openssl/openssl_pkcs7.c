/*
 * Copyright (C) 2012 Martin Willi
 * Copyright (C) 2012 revosec AG
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include "openssl_pkcs7.h"
#include "openssl_util.h"

#include <library.h>
#include <utils/debug.h>
#include <asn1/oid.h>

#include <openssl/cms.h>

typedef struct private_openssl_pkcs7_t private_openssl_pkcs7_t;

/**
 * Private data of an openssl_pkcs7_t object.
 */
struct private_openssl_pkcs7_t {

	/**
	 * Public pkcs7_t interface.
	 */
	pkcs7_t public;

	/**
	 * Type of this container
	 */
	container_type_t type;

	/**
	 * OpenSSL CMS structure
	 */
	CMS_ContentInfo *cms;
};

/**
 * OpenSSL does not allow us to read the signature to verify it with our own
 * crypto API. We define the internal CMS_SignerInfo structure here to get it.
 */
struct CMS_SignerInfo_st {
	long version;
	void *sid;
	X509_ALGOR *digestAlgorithm;
	STACK_OF(X509_ATTRIBUTE) *signedAttrs;
	X509_ALGOR *signatureAlgorithm;
	ASN1_OCTET_STRING *signature;
};

/**
 * We can't include asn1.h, declare function prototype directly
 */
chunk_t asn1_wrap(int, const char *mode, ...);

/**
 * Enumerator for signatures
 */
typedef struct {
	/** implements enumerator_t */
	enumerator_t public;
	/** Stack of signerinfos */
	STACK_OF(CMS_SignerInfo) *signers;
	/** current enumerator position in signers */
	int i;
	/** currently enumerating auth config */
	auth_cfg_t *auth;
	/** full CMS */
	CMS_ContentInfo *cms;
} signature_enumerator_t;

/**
 * Verify signerInfo signature
 */
static auth_cfg_t *verify_signature(CMS_SignerInfo *si, int hash_oid)
{
	enumerator_t *enumerator;
	public_key_t *key;
	certificate_t *cert;
	auth_cfg_t *auth, *found = NULL;
	identification_t *issuer, *serial;
	chunk_t attrs = chunk_empty, sig, attr;
	X509_NAME *name;
	ASN1_INTEGER *snr;
	int i;

	if (CMS_SignerInfo_get0_signer_id(si, NULL, &name, &snr) != 1)
	{
		return NULL;
	}
	issuer = openssl_x509_name2id(name);
	if (!issuer)
	{
		return NULL;
	}
	serial = identification_create_from_encoding(
									ID_KEY_ID, openssl_asn1_str2chunk(snr));

	/* reconstruct DER encoded attributes to verify signature */
	for (i = 0; i < CMS_signed_get_attr_count(si); i++)
	{
		attr = openssl_i2chunk(X509_ATTRIBUTE, CMS_signed_get_attr(si, i));
		attrs = chunk_cat("mm", attrs, attr);
	}
	/* wrap in a ASN1_SET */
	attrs = asn1_wrap(0x31, "m", attrs);

	/* TODO: find a better way to access and verify the signature */
	sig = openssl_asn1_str2chunk(si->signature);
	enumerator = lib->credmgr->create_trusted_enumerator(lib->credmgr,
														KEY_RSA, serial, FALSE);
	while (enumerator->enumerate(enumerator, &cert, &auth))
	{
		if (issuer->equals(issuer, cert->get_issuer(cert)))
		{
			key = cert->get_public_key(cert);
			if (key)
			{
				if (key->verify(key, signature_scheme_from_oid(hash_oid),
								attrs, sig))
				{
					found = auth->clone(auth);
					key->destroy(key);
					break;
				}
				key->destroy(key);
			}
		}
	}
	enumerator->destroy(enumerator);
	issuer->destroy(issuer);
	serial->destroy(serial);
	free(attrs.ptr);

	return found;
}

/**
 * Verify the message digest in the signerInfo attributes
 */
static bool verify_digest(CMS_ContentInfo *cms, CMS_SignerInfo *si, int hash_oid)
{
	ASN1_OCTET_STRING *os, **osp;
	hash_algorithm_t hash_alg;
	chunk_t digest, content, hash;
	hasher_t *hasher;

	os = CMS_signed_get0_data_by_OBJ(si,
				OBJ_nid2obj(NID_pkcs9_messageDigest), -3, V_ASN1_OCTET_STRING);
	if (!os)
	{
		return FALSE;
	}
	digest = openssl_asn1_str2chunk(os);
	osp = CMS_get0_content(cms);
	if (!osp)
	{
		return FALSE;
	}
	content = openssl_asn1_str2chunk(*osp);

	hash_alg = hasher_algorithm_from_oid(hash_oid);
	hasher = lib->crypto->create_hasher(lib->crypto, hash_alg);
	if (!hasher)
	{
		DBG1(DBG_LIB, "hash algorithm %N not supported",
			 hash_algorithm_names, hash_alg);
		return FALSE;
	}
	if (!hasher->allocate_hash(hasher, content, &hash))
	{
		hasher->destroy(hasher);
		return FALSE;
	}
	hasher->destroy(hasher);

	if (!chunk_equals(digest, hash))
	{
		free(hash.ptr);
		DBG1(DBG_LIB, "invalid messageDigest");
		return FALSE;
	}
	free(hash.ptr);
	return TRUE;
}

METHOD(enumerator_t, signature_enumerate, bool,
	signature_enumerator_t *this, auth_cfg_t **out)
{
	if (!this->signers)
	{
		return FALSE;
	}
	while (this->i < sk_CMS_SignerInfo_num(this->signers))
	{
		CMS_SignerInfo *si;
		X509_ALGOR *digest, *sig;
		int hash_oid;

		/* clean up previous round */
		DESTROY_IF(this->auth);
		this->auth = NULL;

		si = sk_CMS_SignerInfo_value(this->signers, this->i++);

		CMS_SignerInfo_get0_algs(si, NULL, NULL, &digest, &sig);
		hash_oid = openssl_asn1_known_oid(digest->algorithm);
		if (openssl_asn1_known_oid(sig->algorithm) != OID_RSA_ENCRYPTION)
		{
			DBG1(DBG_LIB, "only RSA digest encryption supported");
			continue;
		}
		this->auth = verify_signature(si, hash_oid);
		if (!this->auth)
		{
			DBG1(DBG_LIB, "unable to verify pkcs7 attributes signature");
			continue;
		}
		if (!verify_digest(this->cms, si, hash_oid))
		{
			continue;
		}
		*out = this->auth;
		return TRUE;
	}
	return FALSE;
}

METHOD(enumerator_t, signature_destroy, void,
	signature_enumerator_t *this)
{
	DESTROY_IF(this->auth);
	free(this);
}

METHOD(container_t, create_signature_enumerator, enumerator_t*,
	private_openssl_pkcs7_t *this)
{
	signature_enumerator_t *enumerator;

	if (this->type == CONTAINER_PKCS7_SIGNED_DATA)
	{
		INIT(enumerator,
			.public = {
				.enumerate = (void*)_signature_enumerate,
				.destroy = _signature_destroy,
			},
			.cms = this->cms,
			.signers = CMS_get0_SignerInfos(this->cms),
		);
		return &enumerator->public;
	}
	return enumerator_create_empty();
}


METHOD(container_t, get_type, container_type_t,
	private_openssl_pkcs7_t *this)
{
	return this->type;
}

METHOD(pkcs7_t, get_attribute, bool,
	private_openssl_pkcs7_t *this, int oid,
	enumerator_t *enumerator, chunk_t *value)
{
	return FALSE;
}

METHOD(pkcs7_t, create_cert_enumerator, enumerator_t*,
	private_openssl_pkcs7_t *this)
{
	return enumerator_create_empty();
}

METHOD(container_t, get_data, bool,
	private_openssl_pkcs7_t *this, chunk_t *data)
{
	ASN1_OCTET_STRING **os;

	switch (this->type)
	{
		case CONTAINER_PKCS7_DATA:
		case CONTAINER_PKCS7_SIGNED_DATA:
			os = CMS_get0_content(this->cms);
			if (os)
			{
				*data = chunk_clone(openssl_asn1_str2chunk(*os));
				return TRUE;
			}
			break;
		case CONTAINER_PKCS7_ENVELOPED_DATA:
			/* TODO: decrypt */
		default:
			break;
	}
	return FALSE;
}

METHOD(container_t, get_encoding, bool,
	private_openssl_pkcs7_t *this, chunk_t *data)
{
	return FALSE;
}

METHOD(container_t, destroy, void,
	private_openssl_pkcs7_t *this)
{
	CMS_ContentInfo_free(this->cms);
	free(this);
}

/**
 * Generic constructor
 */
static private_openssl_pkcs7_t* create_empty()
{
	private_openssl_pkcs7_t *this;

	INIT(this,
		.public = {
			.container = {
				.get_type = _get_type,
				.create_signature_enumerator = _create_signature_enumerator,
				.get_data = _get_data,
				.get_encoding = _get_encoding,
				.destroy = _destroy,
			},
			.get_attribute = _get_attribute,
			.create_cert_enumerator = _create_cert_enumerator,
		},
	);

	return this;
}

/**
 * Parse a PKCS#7 container
 */
static bool parse(private_openssl_pkcs7_t *this, chunk_t blob)
{
	BIO *bio;

	bio = BIO_new_mem_buf(blob.ptr, blob.len);
	this->cms = d2i_CMS_bio(bio, NULL);
	BIO_free(bio);

	if (!this->cms)
	{
		return FALSE;
	}
	switch (openssl_asn1_known_oid((ASN1_OBJECT*)CMS_get0_type(this->cms)))
	{
		case OID_PKCS7_DATA:
			this->type = CONTAINER_PKCS7_DATA;
			break;
		case OID_PKCS7_SIGNED_DATA:
			this->type = CONTAINER_PKCS7_SIGNED_DATA;
			break;
		case OID_PKCS7_ENVELOPED_DATA:
			this->type = CONTAINER_PKCS7_ENVELOPED_DATA;
			break;
		default:
			return FALSE;
	}

	return TRUE;
}

/**
 * See header
 */
pkcs7_t *openssl_pkcs7_load(container_type_t type, va_list args)
{
	chunk_t blob = chunk_empty;
	private_openssl_pkcs7_t *this;

	while (TRUE)
	{
		switch (va_arg(args, builder_part_t))
		{
			case BUILD_BLOB_ASN1_DER:
				blob = va_arg(args, chunk_t);
				continue;
			case BUILD_END:
				break;
			default:
				return NULL;
		}
		break;
	}
	if (blob.len)
	{
		this = create_empty();
		if (parse(this, blob))
		{
			return &this->public;
		}
		destroy(this);
	}
	return NULL;
}
