/* M0 demo: bouncing sprites over flat decor. Proves the dirty-rect loop.
 * SURF_STATS=1 prints tick-time stats; argv[1] caps frame count (smoke runs).
 */
#include <stdio.h>
#include <stdlib.h>

#include "surfer.h"
#include "hal_sdl.h"
#include "bounce_assets.h"

#define W 1280
#define H 720
#define NBALLS 24

typedef struct {
    surf_node *n;
    int32_t x, y, vx, vy;  /* 16.16 */
    int16_t w, h;
} mover;

static uint32_t rng_state = 0x5eed5u;

static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void mover_step(mover *m)
{
    m->x += m->vx;
    m->y += m->vy;
    int32_t maxx = (W - m->w) << 16, maxy = (H - m->h) << 16;
    if (m->x < 0) { m->x = 0; m->vx = -m->vx; }
    if (m->y < 0) { m->y = 0; m->vy = -m->vy; }
    if (m->x > maxx) { m->x = maxx; m->vx = -m->vx; }
    if (m->y > maxy) { m->y = maxy; m->vy = -m->vy; }
    surf_node_set_pos(m->n, (int16_t)(m->x >> 16), (int16_t)(m->y >> 16));
}

int main(int argc, char **argv)
{
    long max_frames = argc > 1 ? strtol(argv[1], NULL, 10) : 0;
    bool stats = getenv("SURF_STATS") && atoi(getenv("SURF_STATS"));

    const surf_hal *hal = surf_hal_sdl_init(W, H, "surfer M0 — bouncing sprites");
    if (!hal) {
        fprintf(stderr, "bounce: sdl hal init failed\n");
        return 1;
    }
    surf_config cfg = {.max_nodes = 128, .bg = SURF_RGB(24, 26, 32)};
    if (!surf_init(hal, W, H, &cfg)) {
        fprintf(stderr, "bounce: surf_init failed\n");
        return 1;
    }

    surf_image balls_img = {
        .pixels = (void *)bounce_balls_px, .w = BOUNCE_STRIP_W, .h = BOUNCE_STRIP_H,
        .stride = BOUNCE_STRIP_W * 4, .format = SURF_FMT_ARGB8888, .opaque = false,
    };
    surf_image tile_img = {
        .pixels = (void *)bounce_tile_px, .w = BOUNCE_TILE_SIZE, .h = BOUNCE_TILE_SIZE,
        .stride = BOUNCE_TILE_SIZE * 2, .format = SURF_FMT_RGB565, .opaque = true,
    };

    /* flat decor: exercises fill + lets occlusion skip the bg on most rects */
    surf_node_add(surf_screen(), surf_rect_new(0, 0, W, 48, SURF_RGB(38, 42, 52)));
    surf_node_add(surf_screen(), surf_rect_new(0, H - 80, W, 80, SURF_RGB(38, 42, 52)));
    surf_node_add(surf_screen(), surf_rect_new(W / 2 - 2, 48, 4, H - 128, SURF_RGB(52, 58, 72)));

    surf_node *layer = surf_group_new(0, 0);
    surf_node_add(surf_screen(), layer);

    mover movers[NBALLS + 1];
    int nm = 0;

    mover *t = &movers[nm++];
    t->w = t->h = BOUNCE_TILE_SIZE;
    t->x = 100 << 16;
    t->y = 200 << 16;
    t->vx = 3 << 15;
    t->vy = 5 << 14;
    t->n = surf_sprite_new(&tile_img, 100, 200);
    surf_node_add(layer, t->n);

    for (int i = 0; i < NBALLS; i++) {
        mover *m = &movers[nm++];
        m->w = m->h = BOUNCE_BALL_SIZE;
        m->x = (int32_t)(rng() % (W - BOUNCE_BALL_SIZE)) << 16;
        m->y = (int32_t)(rng() % (H - BOUNCE_BALL_SIZE)) << 16;
        m->vx = (int32_t)(rng() % (5 << 16)) + (1 << 16);
        m->vy = (int32_t)(rng() % (5 << 16)) + (1 << 16);
        if (rng() & 1) m->vx = -m->vx;
        if (rng() & 1) m->vy = -m->vy;
        m->n = surf_sprite_new(&balls_img, (int16_t)(m->x >> 16), (int16_t)(m->y >> 16));
        surf_sprite_set_src(m->n, (surf_rect){
            (int16_t)((i % BOUNCE_BALL_FRAMES) * BOUNCE_BALL_SIZE), 0,
            BOUNCE_BALL_SIZE, BOUNCE_BALL_SIZE});
        surf_node_add(layer, m->n);
    }

    long frames = 0;
    uint64_t acc = 0, worst = 0, window_start = hal->now_us();
    int window_frames = 0;

    while (surf_hal_sdl_pump()) {
        for (int i = 0; i < nm; i++)
            mover_step(&movers[i]);

        uint64_t t0 = hal->now_us();
        surf_tick();
        uint64_t dt = hal->now_us() - t0;

        if (stats) {
            acc += dt;
            if (dt > worst) worst = dt;
            window_frames++;
            uint64_t now = hal->now_us();
            if (now - window_start >= 1000000) {
                printf("tick avg %.2f ms  max %.2f ms  %.1f fps\n",
                       acc / 1000.0 / window_frames, worst / 1000.0,
                       window_frames * 1e6 / (double)(now - window_start));
                acc = worst = 0;
                window_frames = 0;
                window_start = now;
            }
        }
        if (max_frames && ++frames >= max_frames)
            break;
    }

    surf_deinit();
    surf_hal_sdl_quit();
    return 0;
}
