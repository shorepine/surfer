/* SDL2 desktop backend. Not part of the binding surface — demos and desktop
 * hosts include this directly. */
#ifndef SURF_HAL_SDL_H
#define SURF_HAL_SDL_H

#include "surfer.h"

const surf_hal *surf_hal_sdl_init(int16_t w, int16_t h, const char *title);
void            surf_hal_sdl_quit(void);
bool            surf_hal_sdl_pump(void);  /* process events; false on quit */

/* Desktop keyboard → textinput plumbing. A physical keyboard is a desktop
 * nicety; on device the on-screen keyboard widget feeds the same node
 * APIs, so none of this is in the hal vtable. */
typedef enum {
    SURF_KEY_TEXT = 0,   /* utf8[] holds the typed text */
    SURF_KEY_LEFT,
    SURF_KEY_RIGHT,
    SURF_KEY_UP,
    SURF_KEY_DOWN,
    SURF_KEY_PGUP,
    SURF_KEY_PGDN,
    SURF_KEY_HOME,
    SURF_KEY_END,
    SURF_KEY_BACKSPACE,
    SURF_KEY_DELETE,
    SURF_KEY_ENTER,
} surf_sdl_key_kind;

typedef struct {
    uint8_t kind;   /* surf_sdl_key_kind */
    bool    shift;  /* extend selection */
    char    utf8[8];
} surf_sdl_key;

bool surf_hal_sdl_poll_key(surf_sdl_key *out);
/* keys currently down (state, not events) — up to max entries */
int  surf_hal_sdl_keys_held(surf_sdl_key *out, int max);

/* debug: write the current framebuffer as a binary PPM (P6) */
bool surf_hal_sdl_dump_ppm(const char *path);

/* debug: write what is actually presented (the streaming texture, read
 * back through the renderer) as a binary PPM. Differs from dump_ppm
 * exactly when present failed to keep the texture coherent with fb. */
bool surf_hal_sdl_dump_screen_ppm(const char *path);

#endif /* SURF_HAL_SDL_H */
