/* Abstract key input: the surfer side of the input-feed boundary
 * (surfer.h). DRIVERS (USB HID, SDL keyboard, ...) push events + held
 * state here; the MicroPython module drains them. No hardware, no
 * platform — a driver has already turned its device into surfer_key.
 *
 * One SPSC ring for events (a driver task produces, the MP thread
 * consumes) plus a small held-set snapshot. Torn/stale reads of the
 * held set are harmless for input (worst case one frame late). */
#include <string.h>

#include "surf_internal.h"

#define KEY_Q_LEN   64
#define KEY_HELD_MAX 8

static struct {
    surfer_key       q[KEY_Q_LEN];
    volatile int     head, tail;
    surfer_key       held[KEY_HELD_MAX];
    volatile int     nheld;
} KB;

void surf_key_event(const surfer_key *k)
{
    int nt = (KB.tail + 1) % KEY_Q_LEN;
    if (nt == KB.head)
        return;                 /* full: drop (typing outran the reader) */
    KB.q[KB.tail] = *k;
    KB.tail = nt;
}

bool surf_key_poll(surfer_key *out)
{
    if (KB.head == KB.tail)
        return false;
    *out = KB.q[KB.head];
    KB.head = (KB.head + 1) % KEY_Q_LEN;
    return true;
}

void surf_key_set_held(const surfer_key *keys, int n)
{
    if (n < 0) n = 0;
    if (n > KEY_HELD_MAX) n = KEY_HELD_MAX;
    for (int i = 0; i < n; i++)
        KB.held[i] = keys[i];
    KB.nheld = n;
}

int surf_key_held(surfer_key *out, int max)
{
    int n = KB.nheld;
    if (n > max) n = max;
    for (int i = 0; i < n; i++)
        out[i] = KB.held[i];
    return n;
}

void surf_key_reset(void)
{
    KB.head = KB.tail = 0;
    KB.nheld = 0;
}
