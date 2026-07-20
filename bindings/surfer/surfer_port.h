/* Platform glue under modsurfer: one implementation per target.
 * port_sdl.c (unix port, desktop window + keyboard) and port_p4.c
 * (esp32 port: DSI panel + GT911 touch + USB host keyboard). */
#ifndef SURFER_PORT_H
#define SURFER_PORT_H

#include "surfer.h"

/* kinds match the module's exported KEY_* constants */
typedef enum {
    SURFER_KEY_TEXT = 0,
    SURFER_KEY_LEFT,
    SURFER_KEY_RIGHT,
    SURFER_KEY_UP,
    SURFER_KEY_DOWN,
    SURFER_KEY_PGUP,
    SURFER_KEY_PGDN,
    SURFER_KEY_HOME,
    SURFER_KEY_END,
    SURFER_KEY_BACKSPACE,
    SURFER_KEY_DELETE,
    SURFER_KEY_ENTER,
} surfer_key_kind;

typedef struct {
    uint8_t kind;
    bool    shift;
    char    utf8[8];
} surfer_key;

/* single_buffer: p4 only — compose directly into the scan buffer (best
 * for full-screen-every-frame animation, DESIGN.md §5.6); ignored on
 * hosts without the choice. */
const surf_hal *surfer_port_init(int16_t w, int16_t h, bool single_buffer);
bool surfer_port_pump(void);   /* false = the host wants to quit */
bool surfer_port_poll_key(surfer_key *out);
bool surfer_port_screenshot(const char *path);
/* Make the framebuffer coherent for CPU reads (fb_read); no-op on hosts
 * whose fb is always CPU-visible. */
void surfer_port_fb_sync_for_read(void);
/* Did a touch device come up? (GT911 probe on p4; SDL mouse counts.) */
bool surfer_port_has_touch(void);
/* Make an image DMA-readable in place (flash .rodata → PSRAM on device;
 * no-op on desktop). Call once per image before first use. */
void surfer_port_prepare_image(surf_image *img);

#endif /* SURFER_PORT_H */
