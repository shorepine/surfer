#include <stdlib.h>
#include <string.h>

#include "mock_hal.h"

mock_op ops[512];
int     nops;
int     test_checks, test_failures;
uint16_t mock_fb[512 * 512];
int16_t  mock_w, mock_h;

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
static void *m_fb_ptr(int32_t *stride)
{
    *stride = mock_w * 2;
    return mock_fb;
}

static void m_scroll_rect(surf_rect r, int16_t dy)
{
    /* record as 'S' with dy in .c, and actually move the pixels so
     * pixel-level assertions can follow a fast scroll */
    ops[nops++] = (mock_op){.op = 'S', .r = r, .c = (surf_color)dy};
    int16_t ady = dy < 0 ? (int16_t)-dy : dy;
    if (dy > 0) {
        for (int y = r.y; y < r.y + r.h - ady; y++)
            memmove(mock_fb + y * mock_w + r.x, mock_fb + (y + ady) * mock_w + r.x,
                    (size_t)r.w * 2);
    } else if (dy < 0) {
        for (int y = r.y + r.h - 1; y >= r.y + ady; y--)
            memmove(mock_fb + y * mock_w + r.x, mock_fb + (y - ady) * mock_w + r.x,
                    (size_t)r.w * 2);
    }
}

const surf_hal mock_hal = {
    m_fill, m_blit, m_blend, m_scale_blit, m_present,
    m_wait_idle, m_now_us, m_poll_touch, m_alloc_image, m_free_image,
    m_fb_ptr, m_scroll_rect,
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
    mock_w = w;
    mock_h = h;
    memset(mock_fb, 0, sizeof mock_fb);
    surf_config cfg = {.max_nodes = max_nodes, .bg = SURF_RGB(0, 0, 0)};
    surf_init(&mock_hal, w, h, &cfg);
    surf_tick();  /* consume the initial full-screen damage */
    nops = 0;
}
