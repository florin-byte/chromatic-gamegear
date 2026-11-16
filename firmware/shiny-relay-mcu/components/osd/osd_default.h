#pragma once

#include "lvgl.h"

void OSD_Default_Init(lv_obj_t *const pScreen);

// Reveal the current page without queuing a menu transition.
void OSD_ReopenCurrent(void);
