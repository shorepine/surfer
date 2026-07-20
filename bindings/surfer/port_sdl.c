/* Desktop (unix port) platform glue: SDL hal + SDL keyboard. */
#include <string.h>

#include "surfer_port.h"
#include "hal_sdl.h"

const surf_hal *surfer_port_init(int16_t w, int16_t h, bool single_buffer)
{
    (void)single_buffer;  /* SDL owns presentation; nothing to choose */
    return surf_hal_sdl_init(w, h, "surfer");
}

bool surfer_port_pump(void)
{
    return surf_hal_sdl_pump();
}

bool surfer_port_poll_key(surfer_key *out)
{
    surf_sdl_key k;  /* kinds are defined to match */
    if (!surf_hal_sdl_poll_key(&k))
        return false;
    out->kind = k.kind;
    out->shift = k.shift;
    memcpy(out->utf8, k.utf8, sizeof out->utf8);
    return true;
}

bool surfer_port_screenshot(const char *path)
{
    return surf_hal_sdl_dump_ppm(path);
}

void surfer_port_prepare_image(surf_image *img)
{
    (void)img;  /* desktop reads .rodata just fine */
}

void surfer_port_fb_sync_for_read(void)
{
    /* software fb: always CPU-coherent */
}

bool surfer_port_has_touch(void)
{
    return true;  /* the mouse */
}

bool surfer_port_touch_info(int *x_max, int *y_max)
{
    *x_max = *y_max = 0;
    return false;
}
