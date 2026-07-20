/* Controller state model: feed/read, 8-way dpad, button bitmask, analog
 * clamp, slot isolation, out-of-range safety. */
#include "mock_hal.h"

void run_pad_tests(void);

void run_pad_tests(void)
{
    surf_pad_reset_all();

    /* dpad: 8-way is two bits; read back the exact mask */
    surf_pad_set_dpad(0, SURF_DPAD_UP | SURF_DPAD_LEFT);
    OK(surf_pad_dpad(0) == (SURF_DPAD_UP | SURF_DPAD_LEFT));
    OK(!(surf_pad_dpad(0) & SURF_DPAD_DOWN));
    surf_pad_set_dpad(0, 0xff);                 /* only the 4 dpad bits kept */
    OK(surf_pad_dpad(0) == 0x0f);

    /* buttons: independent bits */
    surf_pad_set_buttons(0, SURF_BTN_A | SURF_BTN_R);
    OK(surf_pad_buttons(0) == (SURF_BTN_A | SURF_BTN_R));
    OK(surf_pad_buttons(0) & SURF_BTN_A);
    OK(!(surf_pad_buttons(0) & SURF_BTN_B));

    /* analog: stored, and clamped to [-ONE, ONE] */
    surf_pad_set_axis(0, 0, 0, SURF_ONE / 4);
    OK(surf_pad_axis(0, 0, 0) == SURF_ONE / 4);
    surf_pad_set_axis(0, 0, 1, -SURF_ONE * 3);  /* over-range low */
    OK(surf_pad_axis(0, 0, 1) == -SURF_ONE);
    surf_pad_set_axis(0, 1, 0, SURF_ONE * 5);   /* over-range high */
    OK(surf_pad_axis(0, 1, 0) == SURF_ONE);

    /* slots are independent */
    surf_pad_set_buttons(1, SURF_BTN_B);
    OK(surf_pad_buttons(0) == (SURF_BTN_A | SURF_BTN_R));
    OK(surf_pad_buttons(1) == SURF_BTN_B);

    /* out-of-range indices are safe: reads neutral, writes ignored */
    OK(surf_pad_dpad(-1) == 0 && surf_pad_dpad(SURF_MAX_PADS) == 0);
    OK(surf_pad_axis(0, 2, 0) == 0 && surf_pad_axis(0, 0, 5) == 0);
    surf_pad_set_buttons(99, SURF_BTN_A);       /* must not crash */

    /* reset clears one slot, leaves others */
    surf_pad_reset(0);
    OK(surf_pad_dpad(0) == 0 && surf_pad_buttons(0) == 0);
    OK(surf_pad_axis(0, 0, 0) == 0);
    OK(surf_pad_buttons(1) == SURF_BTN_B);
    surf_pad_reset_all();
    OK(surf_pad_buttons(1) == 0);

    /* two sources merge (OR): a gamepad on src 0 and a keyboard on src 1
     * both drive one pad, neither clobbering the other */
    surf_pad_set_dpad_src(0, 0, SURF_DPAD_LEFT);      /* gamepad */
    surf_pad_set_dpad_src(0, 1, SURF_DPAD_UP);        /* keyboard */
    OK(surf_pad_dpad(0) == (SURF_DPAD_LEFT | SURF_DPAD_UP));
    surf_pad_set_buttons_src(0, 0, SURF_BTN_A);
    surf_pad_set_buttons_src(0, 1, SURF_BTN_B);
    OK(surf_pad_buttons(0) == (SURF_BTN_A | SURF_BTN_B));
    surf_pad_set_dpad_src(0, 1, 0);                   /* release keyboard */
    OK(surf_pad_dpad(0) == SURF_DPAD_LEFT);           /* gamepad survives */
    OK(surf_pad_dpad(0) & SURF_DPAD_LEFT);
    surf_pad_set_dpad(0, SURF_DPAD_RIGHT);            /* public = source 0 */
    OK(surf_pad_dpad(0) == SURF_DPAD_RIGHT);
    surf_pad_reset_all();
}
