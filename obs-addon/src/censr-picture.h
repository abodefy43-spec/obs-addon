/* Censr Studio on OBS: the picture of Censr's own OBS, for Studio's window.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The block of shared memory the app makes and the add-on writes the picture into. The app's
 * side (desktop/packages/censr_studio/{linux,windows}/obs_picture.cc) lays it out the same way:
 * this header, then each picture's pixels (RGBA, rows packed), [0] the preview and [1] what's
 * live (the same as the preview outside Studio Mode). */
#define CENSR_PICTURE_MAGIC 0x31505343u /* "CSP1" */
#define CENSR_PICTURE_HEADER 64
#define CENSR_PICTURE_MOST 4096 /* the widest or tallest picture */

struct censr_picture_block {
	uint32_t magic;
	uint32_t width;
	uint32_t height;
	uint32_t pictures;
	/* each picture's count: odd while it's being written, even once whole */
	volatile long seq[2];
};

/* Opens the block CENSR_PICTURE names (Censr starts its own OBS with it) and starts writing the
 * picture into it; nothing when it isn't set. Once OBS has loaded. */
void picture_start(void);

/* Studio Mode came on or went off, or the scene being set up changed. */
void picture_studio_mode(bool on);
void picture_preview_changed(void);

void picture_stop(void);
