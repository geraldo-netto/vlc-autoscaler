// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AUTOUPSCALE_MODULE_H
#define AUTOUPSCALE_MODULE_H

#include <vlc_common.h>

#define UP_CFG_PREFIX "autoupscale-"

int up_autoupscale_open( vlc_object_t *p_this );
void up_autoupscale_close( vlc_object_t *p_this );

#endif
