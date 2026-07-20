/* Shared test scaffolding: op-recording mock hal + check macro. */
#ifndef MOCK_HAL_H
#define MOCK_HAL_H

#include <stdio.h>

#include "surf_internal.h"

typedef struct {
    char       op;  /* 'F' fill, 'B' blit, 'A' blend, 'X' xform, 'P' present */
    surf_rect  r;   /* fill rect; xform: the dst footprint */
    surf_color c;
    const surf_image *img;
    surf_rect  src;
    surf_point dst;
    surf_rect  vis;    /* xform only */
    uint8_t    rot;    /* xform only */
    uint8_t    mirror; /* xform only */
    int        nrects;
} mock_op;

extern mock_op ops[512];
extern int     nops;
extern const surf_hal mock_hal;
extern int     test_checks, test_failures;
extern uint16_t mock_fb[512 * 512];  /* fb_ptr target; stride = init width */
extern int16_t  mock_w, mock_h;

void mock_push_touch(surf_touch t);
void fresh(int16_t w, int16_t h, int max_nodes);  /* re-init, drain, clear ops */
bool rect_eq(surf_rect a, surf_rect b);

#define OK(cond) do {                                                \
        test_checks++;                                               \
        if (!(cond)) {                                               \
            test_failures++;                                         \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
        }                                                            \
    } while (0)

#endif /* MOCK_HAL_H */
