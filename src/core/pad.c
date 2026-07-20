/* Controller state: the normalized layer between input sources and
 * games (DESIGN.md (input model) / surfer.h). Sources (USB gamepad, i2c stick,
 * keyboard map, touch pad) WRITE; games READ. This file holds only the
 * abstract state — no hardware, no platform. Drivers live above it
 * (in the port / tulip5); the frame path never touches pads. */
#include <string.h>

#include "surf_internal.h"

/* Two source channels per slot for the digital controls: reads OR them,
 * so a keyboard (source 1) and a gamepad (source 0) can both drive one
 * pad and either works — neither clobbers the other. Analog sticks have
 * one set (source 0); a keyboard has no analog to contribute. */
#define PAD_SRCS 2
static struct {
    uint8_t  dpad[PAD_SRCS];      /* SURF_DPAD_* bitmask per source */
    uint16_t buttons[PAD_SRCS];   /* SURF_BTN_* bitmask per source */
    int32_t  stick[2][2];         /* [stick][axis] Q16 in [-ONE, ONE] */
} g_pads[SURF_MAX_PADS];

static bool pad_ok(int p) { return p >= 0 && p < SURF_MAX_PADS; }

uint8_t surf_pad_dpad(int p)
{
    return pad_ok(p) ? (g_pads[p].dpad[0] | g_pads[p].dpad[1]) : 0;
}
uint16_t surf_pad_buttons(int p)
{
    return pad_ok(p) ? (g_pads[p].buttons[0] | g_pads[p].buttons[1]) : 0;
}

int32_t surf_pad_axis(int p, int s, int a)
{
    if (!pad_ok(p) || (unsigned)s > 1 || (unsigned)a > 1)
        return 0;
    return g_pads[p].stick[s][a];
}

void surf_pad_set_dpad_src(int p, int src, uint8_t d)
{
    if (pad_ok(p) && (unsigned)src < PAD_SRCS)
        g_pads[p].dpad[src] = d & 0x0f;
}

void surf_pad_set_buttons_src(int p, int src, uint16_t b)
{
    if (pad_ok(p) && (unsigned)src < PAD_SRCS)
        g_pads[p].buttons[src] = b;
}

void surf_pad_set_dpad(int p, uint8_t d) { surf_pad_set_dpad_src(p, 0, d); }
void surf_pad_set_buttons(int p, uint16_t b) { surf_pad_set_buttons_src(p, 0, b); }

void surf_pad_set_axis(int p, int s, int a, int32_t v)
{
    if (!pad_ok(p) || (unsigned)s > 1 || (unsigned)a > 1)
        return;
    if (v > SURF_ONE) v = SURF_ONE;
    if (v < -SURF_ONE) v = -SURF_ONE;
    g_pads[p].stick[s][a] = v;
}

void surf_pad_reset(int p)
{
    if (pad_ok(p))
        memset(&g_pads[p], 0, sizeof g_pads[p]);
}

void surf_pad_reset_all(void)
{
    memset(g_pads, 0, sizeof g_pads);
}
