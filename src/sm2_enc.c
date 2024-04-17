/*
 *  Copyright 2014-2024 The GmSSL Project. All Rights Reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the License); you may
 *  not use this file except in compliance with the License.
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 */


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <gmssl/mem.h>
#include <gmssl/sm2.h>
#include <gmssl/sm2_z256.h>
#include <gmssl/sm3.h>
#include <gmssl/asn1.h>
#include <gmssl/error.h>
#include <gmssl/endian.h>


static int all_zero(const uint8_t *buf, size_t len)
{
	size_t i;
	for (i = 0; i < len; i++) {
		if (buf[i]) {
			return 0;
		}
	}
	return 1;
}

int sm2_do_encrypt_pre_compute(uint64_t k[4], uint8_t C1[64])
{
	SM2_Z256_POINT P;

	// rand k in [1, n - 1]
	do {
		if (sm2_z256_rand_range(k, sm2_z256_order()) != 1) {
			error_print();
			return -1;
		}
	} while (sm2_z256_is_zero(k));

	// output C1 = k * G = (x1, y1)
	sm2_z256_point_mul_generator(&P, k);
	sm2_z256_point_to_bytes(&P, C1);

	return 1;
}

// 和签名不一样，加密的时候要生成 (k, (x1, y1)) ，也就是y坐标也是需要的
// 其中k是要参与计算的，但是 (x1, y1) 不参与计算，输出为 bytes 就可以了
int sm2_do_encrypt(const SM2_KEY *key, const uint8_t *in, size_t inlen, SM2_CIPHERTEXT *out)
{
	sm2_z256_t k;
	SM2_Z256_POINT _P, *P = &_P;
	SM2_Z256_POINT _C1, *C1 = &_C1;
	SM2_Z256_POINT _kP, *kP = &_kP;
	uint8_t x2y2[64];
	SM3_CTX sm3_ctx;

	if (!(SM2_MIN_PLAINTEXT_SIZE <= inlen && inlen <= SM2_MAX_PLAINTEXT_SIZE)) {
		error_print();
		return -1;
	}

	sm2_z256_point_from_bytes(P, (uint8_t *)&key->public_key);

	// S = h * P, check S != O
	// for sm2 curve, h == 1 and S == P
	// SM2_POINT can not present point at infinity, do do nothing here

retry:
	// rand k in [1, n - 1]
	// TODO: set rand_bytes output for testing		
	do {
		if (sm2_z256_rand_range(k, sm2_z256_order()) != 1) {
			error_print();
			return -1;
		}
	} while (sm2_z256_is_zero(k));	//sm2_bn_print(stderr, 0, 4, "k", k);

	// output C1 = k * G = (x1, y1)
	sm2_z256_point_mul_generator(C1, k);
	sm2_z256_point_to_bytes(C1, (uint8_t *)&out->point);

	// k * P = (x2, y2)
	sm2_z256_point_mul(kP, k, P);
	sm2_z256_point_to_bytes(kP, x2y2);

	// t = KDF(x2 || y2, inlen)
	sm2_kdf(x2y2, 64, inlen, out->ciphertext);

	// if t is all zero, retry
	if (all_zero(out->ciphertext, inlen)) {
		goto retry;
	}

	// output C2 = M xor t
	gmssl_memxor(out->ciphertext, out->ciphertext, in, inlen);
	out->ciphertext_size = (uint32_t)inlen;

	// output C3 = Hash(x2 || m || y2)
	sm3_init(&sm3_ctx);
	sm3_update(&sm3_ctx, x2y2, 32);
	sm3_update(&sm3_ctx, in, inlen);
	sm3_update(&sm3_ctx, x2y2 + 32, 32);
	sm3_finish(&sm3_ctx, out->hash);

	gmssl_secure_clear(k, sizeof(k));
	gmssl_secure_clear(kP, sizeof(SM2_Z256_POINT));
	gmssl_secure_clear(x2y2, sizeof(x2y2));
	return 1;
}

int sm2_do_encrypt_fixlen(const SM2_KEY *key, const uint8_t *in, size_t inlen, int point_size, SM2_CIPHERTEXT *out)
{
	unsigned int trys = 200;
	sm2_z256_t k;
	SM2_Z256_POINT _P, *P = &_P;
	SM2_Z256_POINT _C1, *C1 = &_C1;
	SM2_Z256_POINT _kP, *kP = &_kP;
	uint8_t x2y2[64];
	SM3_CTX sm3_ctx;

	if (!(SM2_MIN_PLAINTEXT_SIZE <= inlen && inlen <= SM2_MAX_PLAINTEXT_SIZE)) {
		error_print();
		return -1;
	}

	switch (point_size) {
	case SM2_ciphertext_compact_point_size:
	case SM2_ciphertext_typical_point_size:
	case SM2_ciphertext_max_point_size:
		break;
	default:
		error_print();
		return -1;
	}

	sm2_z256_point_from_bytes(P, (uint8_t *)&key->public_key);

	// S = h * P, check S != O
	// for sm2 curve, h == 1 and S == P
	// SM2_POINT can not present point at infinity, do do nothing here

retry:
	// rand k in [1, n - 1]
	do {
		if (sm2_z256_rand_range(k, sm2_z256_order()) != 1) {
			error_print();
			return -1;
		}
	} while (sm2_z256_is_zero(k));	//sm2_bn_print(stderr, 0, 4, "k", k);

	// output C1 = k * G = (x1, y1)
	sm2_z256_point_mul_generator(C1, k);
	sm2_z256_point_to_bytes(C1, (uint8_t *)&out->point);

	// check fixlen
	if (trys) {
		size_t len = 0;
		asn1_integer_to_der(out->point.x, 32, NULL, &len);
		asn1_integer_to_der(out->point.y, 32, NULL, &len);
		if (len != point_size) {
			trys--;
			goto retry;
		}
	} else {
		gmssl_secure_clear(k, sizeof(k));
		error_print();
		return -1;
	}

	// k * P = (x2, y2)
	sm2_z256_point_mul(kP, k, P);
	sm2_z256_point_to_bytes(kP, x2y2);

	// t = KDF(x2 || y2, inlen)
	sm2_kdf(x2y2, 64, inlen, out->ciphertext);

	// if t is all zero, retry
	if (all_zero(out->ciphertext, inlen)) {
		goto retry;
	}

	// output C2 = M xor t
	gmssl_memxor(out->ciphertext, out->ciphertext, in, inlen);
	out->ciphertext_size = (uint32_t)inlen;

	// output C3 = Hash(x2 || m || y2)
	sm3_init(&sm3_ctx);
	sm3_update(&sm3_ctx, x2y2, 32);
	sm3_update(&sm3_ctx, in, inlen);
	sm3_update(&sm3_ctx, x2y2 + 32, 32);
	sm3_finish(&sm3_ctx, out->hash);

	gmssl_secure_clear(k, sizeof(k));
	gmssl_secure_clear(kP, sizeof(SM2_Z256_POINT));
	gmssl_secure_clear(x2y2, sizeof(x2y2));
	return 1;
}

int sm2_do_decrypt(const SM2_KEY *key, const SM2_CIPHERTEXT *in, uint8_t *out, size_t *outlen)
{
	int ret = -1;
	sm2_z256_t d;
	SM2_Z256_POINT _C1, *C1 = &_C1;
	uint8_t x2y2[64];
	SM3_CTX sm3_ctx;
	uint8_t hash[32];

	// check C1 is on sm2 curve
	sm2_z256_point_from_bytes(C1, (uint8_t *)&in->point);
	if (!sm2_z256_point_is_on_curve(C1)) {
		error_print();
		return -1;
	}

	// check if S = h * C1 is point at infinity
	// this will not happen, as SM2_POINT can not present point at infinity

	// d * C1 = (x2, y2)
	sm2_z256_from_bytes(d, key->private_key);
	sm2_z256_point_mul(C1, d, C1);

	// t = KDF(x2 || y2, klen) and check t is not all zeros
	sm2_z256_point_to_bytes(C1, x2y2);
	sm2_kdf(x2y2, 64, in->ciphertext_size, out);
	if (all_zero(out, in->ciphertext_size)) {
		error_print();
		goto end;
	}

	// M = C2 xor t
	gmssl_memxor(out, out, in->ciphertext, in->ciphertext_size);
	*outlen = in->ciphertext_size;

	// u = Hash(x2 || M || y2)
	sm3_init(&sm3_ctx);
	sm3_update(&sm3_ctx, x2y2, 32);
	sm3_update(&sm3_ctx, out, in->ciphertext_size);
	sm3_update(&sm3_ctx, x2y2 + 32, 32);
	sm3_finish(&sm3_ctx, hash);

	// check if u == C3
	if (memcmp(in->hash, hash, sizeof(hash)) != 0) {
		error_print();
		goto end;
	}
	ret = 1;

end:
	gmssl_secure_clear(d, sizeof(d));
	gmssl_secure_clear(C1, sizeof(SM2_Z256_POINT));
	gmssl_secure_clear(x2y2, sizeof(x2y2));
	return ret;
}


int sm2_ciphertext_to_der(const SM2_CIPHERTEXT *C, uint8_t **out, size_t *outlen)
{
	size_t len = 0;
	if (!C) {
		return 0;
	}
	if (asn1_integer_to_der(C->point.x, 32, NULL, &len) != 1
		|| asn1_integer_to_der(C->point.y, 32, NULL, &len) != 1
		|| asn1_octet_string_to_der(C->hash, 32, NULL, &len) != 1
		|| asn1_octet_string_to_der(C->ciphertext, C->ciphertext_size, NULL, &len) != 1
		|| asn1_sequence_header_to_der(len, out, outlen) != 1
		|| asn1_integer_to_der(C->point.x, 32, out, outlen) != 1
		|| asn1_integer_to_der(C->point.y, 32, out, outlen) != 1
		|| asn1_octet_string_to_der(C->hash, 32, out, outlen) != 1
		|| asn1_octet_string_to_der(C->ciphertext, C->ciphertext_size, out, outlen) != 1) {
		error_print();
		return -1;
	}
	return 1;
}

int sm2_ciphertext_from_der(SM2_CIPHERTEXT *C, const uint8_t **in, size_t *inlen)
{
	int ret;
	const uint8_t *d;
	size_t dlen;
	const uint8_t *x;
	const uint8_t *y;
	const uint8_t *hash;
	const uint8_t *c;
	size_t xlen, ylen, hashlen, clen;

	if ((ret = asn1_sequence_from_der(&d, &dlen, in, inlen)) != 1) {
		if (ret < 0) error_print();
		return ret;
	}
	if (asn1_integer_from_der(&x, &xlen, &d, &dlen) != 1
		|| asn1_length_le(xlen, 32) != 1) {
		error_print();
		return -1;
	}
	if (asn1_integer_from_der(&y, &ylen, &d, &dlen) != 1
		|| asn1_length_le(ylen, 32) != 1) {
		error_print();
		return -1;
	}
	if (asn1_octet_string_from_der(&hash, &hashlen, &d, &dlen) != 1
		|| asn1_check(hashlen == 32) != 1) {
		error_print();
		return -1;
	}
	if (asn1_octet_string_from_der(&c, &clen, &d, &dlen) != 1
	//	|| asn1_length_is_zero(clen) == 1
		|| asn1_length_le(clen, SM2_MAX_PLAINTEXT_SIZE) != 1) {
		error_print();
		return -1;
	}
	if (asn1_length_is_zero(dlen) != 1) {
		error_print();
		return -1;
	}
	memset(C, 0, sizeof(SM2_CIPHERTEXT));
	memcpy(C->point.x + 32 - xlen, x, xlen);
	memcpy(C->point.y + 32 - ylen, y, ylen);
	if (sm2_point_is_on_curve(&C->point) != 1) {
		error_print();
		return -1;
	}
	memcpy(C->hash, hash, hashlen);
	memcpy(C->ciphertext, c, clen);
	C->ciphertext_size = (uint8_t)clen;
	return 1;
}

int sm2_ciphertext_print(FILE *fp, int fmt, int ind, const char *label, const uint8_t *a, size_t alen)
{
	uint8_t buf[512] = {0};
	SM2_CIPHERTEXT *c = (SM2_CIPHERTEXT *)buf;

	if (sm2_ciphertext_from_der(c, &a, &alen) != 1
		|| asn1_length_is_zero(alen) != 1) {
		error_print();
		return -1;
	}
	format_print(fp, fmt, ind, "%s\n", label);
	ind += 4;
	format_bytes(fp, fmt, ind, "XCoordinate", c->point.x, 32);
	format_bytes(fp, fmt, ind, "YCoordinate", c->point.y, 32);
	format_bytes(fp, fmt, ind, "HASH", c->hash, 32);
	format_bytes(fp, fmt, ind, "CipherText", c->ciphertext, c->ciphertext_size);
	return 1;
}

int sm2_encrypt(const SM2_KEY *key, const uint8_t *in, size_t inlen, uint8_t *out, size_t *outlen)
{
	SM2_CIPHERTEXT C;

	if (!key || !in || !out || !outlen) {
		error_print();
		return -1;
	}
	if (!inlen) {
		error_print();
		return -1;
	}

	if (sm2_do_encrypt(key, in, inlen, &C) != 1) {
		error_print();
		return -1;
	}
	*outlen = 0;
	if (sm2_ciphertext_to_der(&C, &out, outlen) != 1) {
		error_print();
		return -1;
	}
	return 1;
}

int sm2_encrypt_fixlen(const SM2_KEY *key, const uint8_t *in, size_t inlen, int point_size, uint8_t *out, size_t *outlen)
{
	SM2_CIPHERTEXT C;

	if (!key || !in || !out || !outlen) {
		error_print();
		return -1;
	}
	if (!inlen) {
		error_print();
		return -1;
	}

	if (sm2_do_encrypt_fixlen(key, in, inlen, point_size, &C) != 1) {
		error_print();
		return -1;
	}
	*outlen = 0;
	if (sm2_ciphertext_to_der(&C, &out, outlen) != 1) {
		error_print();
		return -1;
	}
	return 1;
}

int sm2_decrypt(const SM2_KEY *key, const uint8_t *in, size_t inlen, uint8_t *out, size_t *outlen)
{
	SM2_CIPHERTEXT C;

	if (!key || !in || !out || !outlen) {
		error_print();
		return -1;
	}
	if (sm2_ciphertext_from_der(&C, &in, &inlen) != 1
		|| asn1_length_is_zero(inlen) != 1) {
		error_print();
		return -1;
	}
	if (sm2_do_decrypt(key, &C, out, outlen) != 1) {
		error_print();
		return -1;
	}
	return 1;
}
int sm2_encrypt_init(SM2_ENC_CTX *ctx, const SM2_KEY *sm2_key)
{
	if (!ctx || !sm2_key) {
		error_print();
		return -1;
	}

	memset(ctx, 0, sizeof(*ctx));
	ctx->sm2_key = *sm2_key;

	return 1;
}

int sm2_encrypt_update(SM2_ENC_CTX *ctx, const uint8_t *in, size_t inlen, uint8_t *out, size_t *outlen)
{
	if (!ctx || !outlen) {
		error_print();
		return -1;
	}

	if (ctx->buf_size > SM2_MAX_PLAINTEXT_SIZE) {
		error_print();
		return -1;
	}

	if (!out) {
		*outlen = 0;
		return 1;
	}

	if (in) {
		if (inlen > SM2_MAX_PLAINTEXT_SIZE - ctx->buf_size) {
			error_print();
			return -1;
		}

		memcpy(ctx->buf + ctx->buf_size, in, inlen);
		ctx->buf_size += inlen;
	}

	*outlen = 0;
	return 1;
}

int sm2_encrypt_finish(SM2_ENC_CTX *ctx, const uint8_t *in, size_t inlen, uint8_t *out, size_t *outlen)
{
	if (!ctx || !outlen) {
		error_print();
		return -1;
	}

	if (ctx->buf_size > SM2_MAX_PLAINTEXT_SIZE) {
		error_print();
		return -1;
	}

	if (!out) {
		*outlen = SM2_MAX_CIPHERTEXT_SIZE;
		return 1;
	}

	if (ctx->buf_size) {
		if (in) {
			if (inlen > SM2_MAX_PLAINTEXT_SIZE - ctx->buf_size) {
				error_print();
				return -1;
			}
			memcpy(ctx->buf + ctx->buf_size, in, inlen);
			ctx->buf_size += inlen;
		}
		if (sm2_encrypt(&ctx->sm2_key, ctx->buf, ctx->buf_size, out, outlen) != 1) {
			error_print();
			return -1;
		}
	} else {
		if (!in || !inlen || inlen > SM2_MAX_PLAINTEXT_SIZE) {
			error_print();
			return -1;
		}
		if (sm2_encrypt(&ctx->sm2_key, in, inlen, out, outlen) != 1) {
			error_print();
			return -1;
		}
	}

	return 1;
}

int sm2_decrypt_init(SM2_ENC_CTX *ctx, const SM2_KEY *sm2_key)
{
	if (!ctx || !sm2_key) {
		error_print();
		return -1;
	}

	memset(ctx, 0, sizeof(*ctx));
	ctx->sm2_key = *sm2_key;

	return 1;
}

int sm2_decrypt_update(SM2_ENC_CTX *ctx, const uint8_t *in, size_t inlen, uint8_t *out, size_t *outlen)
{
	if (!ctx || !outlen) {
		error_print();
		return -1;
	}

	if (ctx->buf_size > SM2_MAX_CIPHERTEXT_SIZE) {
		error_print();
		return -1;
	}

	if (!out) {
		*outlen = 0;
		return 1;
	}

	if (in) {
		if (inlen > SM2_MAX_CIPHERTEXT_SIZE - ctx->buf_size) {
			error_print();
			return -1;
		}

		memcpy(ctx->buf + ctx->buf_size, in, inlen);
		ctx->buf_size += inlen;
	}

	*outlen = 0;
	return 1;
}

int sm2_decrypt_finish(SM2_ENC_CTX *ctx, const uint8_t *in, size_t inlen, uint8_t *out, size_t *outlen)
{
	if (!ctx || !outlen) {
		error_print();
		return -1;
	}

	if (ctx->buf_size > SM2_MAX_CIPHERTEXT_SIZE) {
		error_print();
		return -1;
	}

	if (!out) {
		*outlen = SM2_MAX_PLAINTEXT_SIZE;
		return 1;
	}

	if (ctx->buf_size) {
		if (in) {
			if (inlen > SM2_MAX_CIPHERTEXT_SIZE - ctx->buf_size) {
				error_print();
				return -1;
			}
			memcpy(ctx->buf + ctx->buf_size, in, inlen);
			ctx->buf_size += inlen;
		}
		if (sm2_decrypt(&ctx->sm2_key, ctx->buf, ctx->buf_size, out, outlen) != 1) {
			error_print();
			return -1;
		}
	} else {
		if (!in || !inlen || inlen > SM2_MAX_CIPHERTEXT_SIZE) {
			error_print();
			return -1;
		}
		if (sm2_decrypt(&ctx->sm2_key, in, inlen, out, outlen) != 1) {
			error_print();
			return -1;
		}
	}

	return 1;
}