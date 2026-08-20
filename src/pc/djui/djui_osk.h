#pragma once
#include "djui.h"

#ifdef HANDHELD
void djui_osk_update(void);
void djui_osk_render(void);
#else
static inline void djui_osk_update(void) {}
static inline void djui_osk_render(void) {}
#endif
