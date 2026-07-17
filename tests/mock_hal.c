#include <stdlib.h>

#include "mock_hal.h"

mock_op ops[512];
int     nops;
int     test_checks, test_failures;

static surf_touch tq[64];
static int tq_r, tq_w;

static void m_fill(surf_rect dst, surf_color c)
{
    ops[nops++] = (mock_op){.op = 'F', .r = dst, .c = c};
}
static void m_blit(const surf_image *src, surf_rect sr, surf_point dst)
{
    ops[nops++] = (mock_op){.op = 'B', .img = src, .src = sr, .dst = dst};
}
static void m_blend(const surf_image *src, surf_rect sr, surf_point dst, uint8_t opa)
{
    (void)opa;
    ops[nops++] = (mock_op){.op = 'A', .img = src, .src = sr, .dst = dst};
}
static void m_scale_blit(const surf_image *s, surf_rect a, surf_rect b)
{
    (void)s; (void)a; (void)b;
}
static void m_present(const surf_rect *dirty, int n)
{
    (void)dirty;
    ops[nops++] = (mock_op){.op = 'P', .nrects = n};
}
static void m_wait_idle(void) {}
static uint64_t m_now_us(void) { return 0; }
static bool m_poll_touch(surf_touch *out)
{
    if (tq_r == tq_w)
        return false;
    *out = tq[tq_r++];
    if (tq_r == tq_w)
        tq_r = tq_w = 0;
    return true;
}
static void *m_alloc_image(size_t b) { return malloc(b); }
static void m_free_image(void *p) { free(p); }

const surf_hal mock_hal = {
    m_fill, m_blit, m_blend, m_scale_blit, m_present,
    m_wait_idle, m_now_us, m_poll_touch, m_alloc_image, m_free_image,
};

void mock_push_touch(surf_touch t)
{
    tq[tq_w++] = t;
}

bool rect_eq(surf_rect a, surf_rect b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

void fresh(int16_t w, int16_t h, int max_nodes)
{
    surf_deinit();
    tq_r = tq_w = 0;
    surf_config cfg = {.max_nodes = max_nodes, .bg = SURF_RGB(0, 0, 0)};
    surf_init(&mock_hal, w, h, &cfg);
    surf_tick();  /* consume the initial full-screen damage */
    nops = 0;
}
