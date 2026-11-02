/* SHA-256 and HMAC-SHA256 (FIPS 180-4, RFC 2104), for the link's two-way proof: OBS has none a
 * plugin can use.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* HMAC-SHA256 of [message] with [key], as 64 lowercase hex characters and a NUL in [hex]. */
void censr_hmac_hex(const char *key, size_t key_size, const char *message, size_t message_size, char hex[65]);

/* Whether [a] and [b] (each [size] bytes) are the same, taking as long whatever they hold. */
int censr_same(const char *a, const char *b, size_t size);
