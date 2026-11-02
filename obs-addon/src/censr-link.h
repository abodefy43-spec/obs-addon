/* The link to the Censr app on this computer (engine/obs_link.py is Censr's side of it).
 *
 * Censr writes a small file only this user can read: a port on this computer and a secret made
 * fresh each time. The link reads it, connects, says the secret, then hands each message to the
 * callbacks below: the sound's shape, the sound itself, and the word to take over OBS's own
 * sound or give it back. It tries again every second while Censr isn't there.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct censr_link_events {
	void (*shape)(uint32_t rate, uint32_t channels, void *data);
	void (*audio)(const uint8_t *samples, size_t bytes, void *data); /* 16-bit, interleaved */
	void (*take)(bool computer, int delay_ms, void *data);
	void (*give_back)(void *data);
	void (*caption)(const char *text, int after_ms, void *data);
	void (*connected)(bool on, void *data);
	void *data;
};

struct censr_link;

struct censr_link *censr_link_start(const struct censr_link_events *events);
void censr_link_stop(struct censr_link *link);

/* Tells Censr how things are: whether OBS's own sound is taken over now. */
void censr_link_say(struct censr_link *link, bool taken);
