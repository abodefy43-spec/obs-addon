/* Censr Studio on OBS: Studio's recording, stream and replay buffer, in Censr's own OBS.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

/* Offers Studio's "censr" requests through OBS's remote control, in Censr's own OBS only (Censr
 * starts it with CENSR_STUDIO=1). From obs_module_post_load, once OBS's remote control is loaded. */
void studio_load(void);

/* OBS is closing: everything Studio started is stopped, and no more is said to the remote
 * control, which closes too. */
void studio_exit(void);

/* Lets go of what's left, as the add-on unloads. */
void studio_unload(void);
