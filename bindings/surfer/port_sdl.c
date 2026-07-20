/* Desktop (unix port) platform glue: SDL hal + SDL keyboard. */
#include <string.h>
#ifndef __EMSCRIPTEN__
#include <sys/resource.h>
#include <sys/time.h>
#endif

#include "surfer_port.h"
#include "hal_sdl.h"

const surf_hal *surfer_port_init(int16_t w, int16_t h, bool single_buffer)
{
    (void)single_buffer;  /* SDL owns presentation; nothing to choose */
    return surf_hal_sdl_init(w, h, "surfer");
}

/* The desktop's "input driver": each pump, drain SDL key events and
 * snapshot held keys into surfer's abstract feed API (surf_key_*). This
 * is the same contract a device USB driver uses — surfer never sees
 * SDL. */
bool surfer_port_pump(void)
{
    bool ok = surf_hal_sdl_pump();
    surf_sdl_key k;
    while (surf_hal_sdl_poll_key(&k)) {
        surfer_key e = {.kind = k.kind, .shift = k.shift};
        memcpy(e.utf8, k.utf8, sizeof e.utf8);
        surf_key_event(&e);
    }
    surf_sdl_key h[8];
    int n = surf_hal_sdl_keys_held(h, 8);
    surfer_key held[8];
    for (int i = 0; i < n; i++) {
        held[i] = (surfer_key){.kind = h[i].kind, .shift = h[i].shift};
        memcpy(held[i].utf8, h[i].utf8, sizeof held[i].utf8);
    }
    surf_key_set_held(held, n);
    return ok;
}

/* desktop: one number — process cpu time over wall time (no per-core
 * story for a windowed process); web has neither */
int surfer_port_cpu_usage(float *pct, int max)
{
#ifndef __EMSCRIPTEN__
    if (max < 1)
        return 0;
    static int64_t last_cpu_us, last_wall_us;
    struct rusage ru;
    struct timeval tv;
    if (getrusage(RUSAGE_SELF, &ru) != 0)
        return 0;
    gettimeofday(&tv, NULL);
    int64_t cpu = (int64_t)ru.ru_utime.tv_sec * 1000000 + ru.ru_utime.tv_usec +
                  (int64_t)ru.ru_stime.tv_sec * 1000000 + ru.ru_stime.tv_usec;
    int64_t wall = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
    int64_t dc = cpu - last_cpu_us, dw = wall - last_wall_us;
    last_cpu_us = cpu;
    last_wall_us = wall;
    if (dw <= 0)
        return 0;
    float busy = 100.0f * (float)dc / (float)dw;
    if (busy < 0.0f) busy = 0.0f;
    if (busy > 100.0f) busy = 100.0f;
    pct[0] = busy;
    return 1;
#else
    (void)pct; (void)max;
    return 0;
#endif
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
