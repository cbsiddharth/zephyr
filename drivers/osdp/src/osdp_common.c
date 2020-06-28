/*
 * Copyright (c) 2020 Siddharth Chandrasekaran <siddharth@embedjournal.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <ctype.h>

#include <device.h>
#include <sys/crc.h>
#include <logging/log.h>

#if CONFIG_OSDP_SC_ENABLED
#include <crypto/cipher.h>
#include <random/rand32.h>
#endif

#include "osdp_common.h"

LOG_MODULE_DECLARE(osdp, CONFIG_OSDP_LOG_LEVEL);

void osdp_dump(const char *head, uint8_t *buf, int len)
{
	LOG_HEXDUMP_DBG(buf, len, head);
}

uint16_t compute_crc16(const uint8_t *buf, size_t len)
{
	return crc16_itu_t(0x1D0F, buf, len);
}

millis_t millis_now()
{
	return (millis_t) k_uptime_get();
}

millis_t millis_since(millis_t last)
{
	millis_t tmp = last;

	return (millis_t) k_uptime_delta(&tmp);
}

K_MEM_SLAB_DEFINE(osdp_cmd_slab, sizeof(struct osdp_cmd),
		  32 * CONFIG_OSDP_NUM_CONNECTED_PD, 4);

struct osdp_cmd *osdp_cmd_alloc()
{
	struct osdp_cmd *cmd = NULL;

	if (k_mem_slab_alloc(&osdp_cmd_slab, (void **)&cmd, K_MSEC(100))) {
		LOG_ERR("Memory allocation time-out");
		return NULL;
	}
	return cmd;
}

void osdp_cmd_free(struct osdp_cmd *cmd)
{
	k_mem_slab_free(&osdp_cmd_slab, (void **)&cmd);
}

#if CONFIG_OSDP_SC_ENABLED
static inline int hex2int(char ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	ch &= ~0x20; // to upper case.
	if (ch >= 'A' && ch <= 'F')
		return ch - 'A' + 10;
	return -1;
}

int hstrtoa(uint8_t *arr, int max_len, const char *hstr)
{
	int i, len, nib1, nib2;

	len = strlen(hstr);
	/* len must be even, non-zero and len / 2 <= max_len. */
	if ((len & 1) == 1 || len == 0 || (len / 2) > max_len) {
		return -1;
	}
	len = len / 2;
	for (i = 0; i < len; i++) {
		nib1 = hex2int(hstr[(i * 2) + 0]);
		nib2 = hex2int(hstr[(i * 2) + 1]);
		if (nib1 == -1 || nib2 == -1)
			return -1;
		arr[i] = nib1 << 4 | nib2;
	}
	return len;
}

void osdp_encrypt(uint8_t *key, uint8_t *iv, uint8_t *data, int len)
{
	struct device *dev;
	struct cipher_ctx ctx = {
		.keylen = 16,
		.key.bit_stream = key,
		.flags = CAP_NO_IV_PREFIX
	};
	struct cipher_pkt encrypt = {
		.in_buf = data,
		.in_len = len,
		.out_buf = data,
		.out_len = len
	};

	dev = device_get_binding(CONFIG_OSDP_CRYPTO_DRV_NAME);
	if (dev == NULL) {
		LOG_ERR("Failed to get crypto dev binding!");
		return;
	}

	if (iv != NULL) {
		if (cipher_begin_session(dev, &ctx,
					 CRYPTO_CIPHER_ALGO_AES,
					 CRYPTO_CIPHER_MODE_CBC,
					 CRYPTO_CIPHER_OP_ENCRYPT)) {
			LOG_ERR("Failed at cipher_begin_session");
			return;
		}

		if (cipher_cbc_op(&ctx, &encrypt, iv)) {
			LOG_ERR("CBC ENCRYPT - Failed");
		}
	} else {
		if (cipher_begin_session(dev, &ctx,
					 CRYPTO_CIPHER_ALGO_AES,
					 CRYPTO_CIPHER_MODE_ECB,
					 CRYPTO_CIPHER_OP_ENCRYPT)) {
			LOG_ERR("Failed at cipher_begin_session");
			return;
		}

		if (cipher_block_op(&ctx, &encrypt)) {
			LOG_ERR("ECB ENCRYPT - Failed");
		}
	}
	cipher_free_session(dev, &ctx);
}

void osdp_decrypt(uint8_t *key, uint8_t *iv, uint8_t *data, int len)
{
	struct device *dev;
	struct cipher_ctx ctx = {
		.keylen = 16,
		.key.bit_stream = key,
		.flags = CAP_NO_IV_PREFIX
	};
	struct cipher_pkt decrypt = {
		.in_buf = data,
		.in_len = len,
		.out_buf = data,
		.out_len = len
	};

	dev = device_get_binding(CONFIG_OSDP_CRYPTO_DRV_NAME);
	if (dev == NULL) {
		LOG_ERR("Failed to get crypto dev binding!");
		return;
	}

	if (iv != NULL) {
		if (cipher_begin_session(dev, &ctx,
					 CRYPTO_CIPHER_ALGO_AES,
					 CRYPTO_CIPHER_MODE_CBC,
					 CRYPTO_CIPHER_OP_DECRYPT)) {
			LOG_ERR("Failed at cipher_begin_session");
			return;
		}
		if (cipher_cbc_op(&ctx, &decrypt, iv)) {
			LOG_ERR("CBC DECRYPT - Failed");
		}
	} else {
		if (cipher_begin_session(dev, &ctx,
					 CRYPTO_CIPHER_ALGO_AES,
					 CRYPTO_CIPHER_MODE_ECB,
					 CRYPTO_CIPHER_OP_DECRYPT)) {
			LOG_ERR("Failed at cipher_begin_session");
			return;
		}
		if (cipher_block_op(&ctx, &decrypt)) {
			LOG_ERR("ECB DECRYPT - Failed");
		}
	}
	cipher_free_session(dev, &ctx);
}

void osdp_fill_random(uint8_t *buf, int len)
{
	sys_rand_get(buf, len);
}
#endif
