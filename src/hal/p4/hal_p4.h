/* ESP32-P4 backend: PPA fill/SRM/blend engines into an RGB565 compose
 * buffer in PSRAM; present copies dirty rects to the DSI scanout fb. */
#ifndef SURF_HAL_P4_H
#define SURF_HAL_P4_H

#include "surfer.h"
#include "esp_lcd_types.h"

/* Board glue polls its own touch controller; the hal turns level+position
 * into DOWN/MOVE/UP events. Return true while pressed. */
typedef bool (*surf_p4_touch_poll_fn)(int16_t *x, int16_t *y);

typedef struct {
    esp_lcd_panel_handle_t panel;    /* NULL → headless (bench only) */
    void                  *scan_fb;  /* DSI scanout framebuffer, RGB565 */
    int16_t                w, h;
    surf_p4_touch_poll_fn  touch_poll;  /* optional */
    /* true: composite straight into scan_fb, present is free (tear risk on
     * large updates); false: separate compose fb + damage copy on present.
     * The M2 buffering benchmark — measure both (DESIGN.md §2.3). */
    bool                   single_buffer;
} surf_hal_p4_cfg;

const surf_hal *surf_hal_p4_init(const surf_hal_p4_cfg *cfg);

/* Cache writeback after CPU writes to an image (the LVGL-PPA class of bug,
 * owned here once — see DESIGN.md §2.1). Call after filling any buffer the
 * PPA will read. */
void surf_hal_p4_sync(const void *buf, size_t bytes);

#endif /* SURF_HAL_P4_H */
