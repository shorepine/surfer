#include "surf_internal.h"

/* Touch → hit test → nearest ancestor that wants the gesture: a node with
 * a handler, or a scrollable scrollview (whichever comes first walking
 * up). The winner holds that CONTACT from DOWN to UP (DESIGN.md §2.6).
 *
 * When a handler wins inside a scrollview, the scrollview waits: once the
 * finger travels past STEAL_PX along an axis it can scroll, it steals the
 * gesture — the handler gets a synthetic UP and the drag becomes a
 * scroll. Handlers that own their drags (sliders, knobs, textinput
 * selection) set SURF_NF_GRAB and are never stolen from.
 *
 * CAPTURE IS PER CONTACT. There used to be one `capture` for the whole
 * scene, which meant the first finger down owned the machine: three
 * fingers on three faders moved one fader. Every piece of that state —
 * the captured node, the scrollview waiting to steal, the position the
 * gesture started from — is per finger now, because all three are
 * answers to "what is THIS finger doing".
 *
 * A hal with no multitouch says nothing about contacts and its events
 * carry id 0, which lands in one slot and behaves exactly as before. */

#define STEAL_PX 8

surf_contact *surf_contact_find(uint8_t id)
{
    for (int i = 0; i < SURF_MAX_CONTACTS; i++)
        if (surf_g.contacts[i].used && surf_g.contacts[i].id == id)
            return &surf_g.contacts[i];
    return NULL;
}

/* A DOWN for an id already in flight REPLACES it rather than opening a
 * second slot: a controller that misses an UP (the GT911 does, when a
 * finger lifts during an i2c hiccup) would otherwise leak slots until
 * the table is full and further fingers are silently ignored. */
static surf_contact *contact_open(uint8_t id)
{
    surf_contact *c = surf_contact_find(id);
    if (!c) {
        for (int i = 0; i < SURF_MAX_CONTACTS; i++) {
            if (!surf_g.contacts[i].used) {
                c = &surf_g.contacts[i];
                break;
            }
        }
    }
    if (!c)
        return NULL;             /* more fingers than we follow: ignore */
    c->used = true;
    c->id = id;
    c->capture = NULL;
    c->steal_sv = NULL;
    return c;
}

static void deliver(surf_node *n, const surf_touch *t)
{
    if (n->type == SURF_NODE_SCROLLVIEW && !n->on_touch)
        surf_scroll_touch(n, t);
    else if (n->on_touch)
        n->on_touch(n, t, n->touch_user);
}

/* A WHEEL (or a two-finger trackpad push) over a scrollview.
 *
 * Drag was the only way to scroll anything here, which is right for a
 * touchscreen and wrong for the two targets that have a mouse: on a
 * laptop the natural gesture is two fingers, and it did nothing at all.
 * The hal turns its wheel event into this; the walk is the dispatch's
 * own — the first scrollable ancestor of whatever is under the pointer,
 * so a list inside a panel scrolls the list and a page behind it stays
 * put.
 *
 * It moves the view directly rather than faking a drag: a wheel has no
 * press and no release, and a synthetic down/up pair would light up
 * every row it passed over.
 *
 * AND WHAT NO SCROLLVIEW TAKES IS QUEUED FOR THE APPLICATION, because
 * scrolling a list is not the only thing a wheel can mean: over a
 * picture it is a zoom, over a value it is a step. That is touch's own
 * bargain — the widget under the pointer wins and the app gets what
 * nothing claimed — so a dialog's file list still scrolls under the
 * wheel while the same gesture over the app behind it reaches the app.
 * One ring, the key queue's shape, drained by surf_wheel_poll. */
#define WHEEL_Q_LEN 32

static struct {
    surf_wheel   q[WHEEL_Q_LEN];
    volatile int head, tail;
} WH;

void surf_input_wheel(int16_t x, int16_t y, int16_t dx, int16_t dy)
{
    for (surf_node *n = surf_hit_test(x, y); n; n = n->parent) {
        if (n->type != SURF_NODE_SCROLLVIEW)
            continue;
        bool cx = surf_scroll_can_x(n), cy = surf_scroll_can_y(n);
        if (!cx && !cy)
            continue;
        surf_point off = surf_scrollview_offset(n);
        surf_scrollview_set_offset(n, (int16_t)(cx ? off.x + dx : off.x),
                                   (int16_t)(cy ? off.y + dy : off.y));
        return;
    }
    int nt = (WH.tail + 1) % WHEEL_Q_LEN;
    if (nt == WH.head)
        return;              /* full: drop, the way typing outrunning the
                              * reader drops a key. A wheel nobody is
                              * draining is a wheel nobody wants. */
    WH.q[WH.tail] = (surf_wheel){x, y, dx, dy};
    WH.tail = nt;
}

bool surf_wheel_poll(surf_wheel *out)
{
    if (WH.head == WH.tail)
        return false;
    *out = WH.q[WH.head];
    WH.head = (WH.head + 1) % WHEEL_Q_LEN;
    return true;
}

void surf_wheel_reset(void)
{
    WH.head = WH.tail = 0;
}

void surf_input_dispatch(const surf_touch *t)
{
    switch (t->phase) {
    case SURF_TOUCH_DOWN: {
        surf_contact *ct = contact_open(t->id);
        if (!ct)
            return;
        ct->down_x = t->x;
        ct->down_y = t->y;

        surf_node *n = surf_hit_test(t->x, t->y);
        for (; n; n = n->parent) {
            if (n->on_touch)
                break;
            if (n->type == SURF_NODE_SCROLLVIEW &&
                (surf_scroll_can_x(n) || surf_scroll_can_y(n)))
                break;
        }
        if (!n)
            return;              /* the slot stays live but captures nothing */
        ct->capture = n;
        if (n->type == SURF_NODE_SCROLLVIEW && !n->on_touch) {
            surf_scroll_begin(n, t);
            return;
        }
        n->on_touch(n, t, n->touch_user);
        /* THE HANDLER MAY HAVE DESTROYED THE TREE IT WAS IN. node_free()
         * clears any contact that pointed at a freed node, so if this
         * contact no longer holds `n`, `n` and everything above it are
         * gone and walking its parents would be a use-after-free. */
        if (ct->capture != n)
            return;
        if (!(n->flags & SURF_NF_GRAB)) {
            for (surf_node *p = n->parent; p; p = p->parent) {
                if (p->type == SURF_NODE_SCROLLVIEW &&
                    (surf_scroll_can_x(p) || surf_scroll_can_y(p))) {
                    ct->steal_sv = p;
                    break;
                }
            }
        }
        break;
    }
    case SURF_TOUCH_MOVE: {
        surf_contact *ct = surf_contact_find(t->id);
        if (!ct || !ct->capture)
            return;
        surf_node *c = ct->capture;
        surf_node *sv = ct->steal_sv;
        if (sv) {
            int dx = t->x - ct->down_x, dy = t->y - ct->down_y;
            int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
            bool steal = (ady >= adx) ? (ady > STEAL_PX && surf_scroll_can_y(sv))
                                      : (adx > STEAL_PX && surf_scroll_can_x(sv));
            if (steal) {
                surf_touch up = {t->x, t->y, SURF_TOUCH_UP, t->id};
                deliver(c, &up);
                /* ...and the same hazard, in its worst form: a row that
                 * reads this synthetic UP as a TAP can tear down the
                 * very list it is in. `sv` and `c` are LOCAL COPIES made
                 * before the callback, so node_free()'s clearing of the
                 * contact does not reach them — this is the check that
                 * does. Without it the next line captures a freed node
                 * and every move after it writes through the pointer.
                 *
                 * tulip2's launcher found this on Safari, where the
                 * recycled slot was not benign: a drag of nine pixels
                 * took the whole machine down. */
                if (ct->capture != c || ct->steal_sv != sv) {
                    ct->capture = NULL;
                    ct->steal_sv = NULL;
                    return;
                }
                ct->capture = sv;
                ct->steal_sv = NULL;
                surf_scroll_begin(sv, t);
                return;
            }
        }
        deliver(c, t);
        break;
    }
    case SURF_TOUCH_UP: {
        surf_contact *ct = surf_contact_find(t->id);
        if (!ct)
            return;
        surf_node *c = ct->capture;
        ct->used = false;
        ct->capture = NULL;
        ct->steal_sv = NULL;
        if (c)
            deliver(c, t);
        break;
    }
    }
}

/* The platform screen keyboard. -1 asks, 1 summons, 0 dismisses; the
 * return is what is actually shown, or -1 where the platform has no
 * screen keyboard at all — which is this default's whole answer. The
 * SDL hal overrides it (weak/strong at link) for the one platform that
 * has one, iOS; a device hal with a glass keyboard of its own would do
 * the same. */
__attribute__((weak)) int surf_screen_keyboard(int op)
{
    (void)op;
    return -1;
}

/* HOST CHROME, for a host that draws its own controls outside the
 * machine's screen. `surf_host_chrome_q16` is how much of the
 * window height it wants kept clear at the bottom (and
 * `surf_host_chrome_top_q16` at the top: a notch or an island), as a
 * Q16 FRACTION
 * — a fraction rather than points because the host measures in its own
 * coordinate space and SDL's window height does not always agree with
 * it (see the hal's note); `surf_host_ctrl_latch` is a one-shot
 * Ctrl for a keyboard that has no ctrl key, consumed by the next
 * character. Both are WEAK and zero here — a host that draws no chrome
 * (every desktop, the browser, the panel) links this and nothing
 * changes. tulip2's iOS build defines them strongly in
 * drivers/ios_bar.m, which is the only reason they exist. */
__attribute__((weak)) int surf_host_chrome_q16 = 0;
__attribute__((weak)) int surf_host_chrome_top_q16 = 0;
__attribute__((weak)) int surf_host_ctrl_latch = 0;
