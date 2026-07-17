/* SDL2 desktop backend. Not part of the binding surface — demos and desktop
 * hosts include this directly. */
#ifndef SURF_HAL_SDL_H
#define SURF_HAL_SDL_H

#include "surfer.h"

const surf_hal *surf_hal_sdl_init(int16_t w, int16_t h, const char *title);
void            surf_hal_sdl_quit(void);
bool            surf_hal_sdl_pump(void);  /* process events; false on quit */

#endif /* SURF_HAL_SDL_H */
