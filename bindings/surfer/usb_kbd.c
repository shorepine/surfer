/* USB-host HID input, raw usb_host API (no managed components, so it
 * builds inside the MicroPython esp32 port). One task drives the host
 * library and our client. Supports several devices at once (a keyboard
 * AND a gamepad behind a hub): each opened device gets a slot with its
 * own interrupt-IN transfer. Two device kinds are understood:
 *   - boot-protocol keyboards  -> surfer_key events + held state
 *   - XInput gamepads (class ff/5d/01, fixed 20-byte report) -> pad 0
 * Runs on the P4's Full-Speed OTG (peripheral 1, internal PHY on GPIO
 * 26/27) so a hub enumerates at FS and its FS devices need no
 * Transaction Translator (which IDF's hub driver lacks). */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "usb/usb_host.h"

#include "surfer.h"   /* surf_pad_* controller API */
#include "usb_kbd.h"

#define TAG "surfer_kbd"
#define MAX_HID_DEV 4

typedef struct {
    usb_device_handle_t dev;      /* NULL = free slot */
    uint8_t             addr;
    uint8_t             iface;
    usb_transfer_t     *xfer;
    bool                gamepad;  /* else boot keyboard */
    /* keyboard state (HID boot reports carry state, not repeats) */
    uint8_t             prev[6];
    uint8_t             mods;
    uint8_t             held_usage, held_mods;
    int64_t             held_since_us, last_rep_us;
} hid_dev_t;

static struct {
    usb_host_client_handle_t client;
    QueueHandle_t            q;      /* surfer_key events (all keyboards) */
    /* NEW_DEV addresses waiting to be opened. A hub enumerates its
     * downstream devices as separate events, so a single slot would
     * drop one — this is a small ring. */
    uint8_t                  addr_q[8];
    volatile uint8_t         addr_head, addr_tail;
    hid_dev_t                devs[MAX_HID_DEV];
} K;

#define KBD_REPEAT_DELAY_US  350000
#define KBD_REPEAT_RATE_US    55000

/* HID usage id → lowercase ascii (0 = not printable) for 0x04..0x38 */
static const char base_map[0x39] = {
    [0x04] = 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
    'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    [0x1e] = '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    [0x2c] = ' ',
    [0x2d] = '-', '=', '[', ']', '\\',
    [0x33] = ';', '\'', '`', ',', '.', '/',
};
static const char shift_map[0x39] = {
    [0x04] = 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
    'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    [0x1e] = '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
    [0x2c] = ' ',
    [0x2d] = '_', '+', '{', '}', '|',
    [0x33] = ':', '"', '~', '<', '>', '?',
};

/* usage id + modifiers → key; false when the usage maps to nothing */
static bool usage_to_key(uint8_t u, uint8_t mods, surfer_key *out)
{
    bool shift = (mods & 0x22) != 0;
    memset(out, 0, sizeof *out);
    out->shift = shift;
    switch (u) {
    case 0x28: out->kind = SURFER_KEY_ENTER; return true;
    case 0x2a: out->kind = SURFER_KEY_BACKSPACE; return true;
    case 0x2b: out->kind = SURFER_KEY_TEXT;  /* tab → space-ish */
               out->shift = false;
               out->utf8[0] = ' ';
               return true;
    case 0x4a: out->kind = SURFER_KEY_HOME; return true;
    case 0x4b: out->kind = SURFER_KEY_PGUP; return true;
    case 0x4c: out->kind = SURFER_KEY_DELETE; return true;
    case 0x4d: out->kind = SURFER_KEY_END; return true;
    case 0x4e: out->kind = SURFER_KEY_PGDN; return true;
    case 0x4f: out->kind = SURFER_KEY_RIGHT; return true;
    case 0x50: out->kind = SURFER_KEY_LEFT; return true;
    case 0x51: out->kind = SURFER_KEY_DOWN; return true;
    case 0x52: out->kind = SURFER_KEY_UP; return true;
    }
    if (u < sizeof base_map) {
        char ch = shift ? shift_map[u] : base_map[u];
        if (ch) {
            out->kind = SURFER_KEY_TEXT;
            out->utf8[0] = ch;
            return true;
        }
    }
    return false;
}

static void handle_usage(uint8_t u, uint8_t mods)
{
    surfer_key k;
    if (usage_to_key(u, mods, &k))
        xQueueSend(K.q, &k, 0);
}

/* XInput 20-byte input report -> pad slot 0 */
static void gamepad_report(const uint8_t *r)
{
    uint8_t b1 = r[2], b2 = r[3];
    uint8_t dpad = 0;
    if (b1 & 0x01) dpad |= SURF_DPAD_UP;
    if (b1 & 0x02) dpad |= SURF_DPAD_DOWN;
    if (b1 & 0x04) dpad |= SURF_DPAD_LEFT;
    if (b1 & 0x08) dpad |= SURF_DPAD_RIGHT;
    uint16_t btn = 0;
    if (b1 & 0x10) btn |= SURF_BTN_START;
    if (b1 & 0x20) btn |= SURF_BTN_SELECT;   /* Back */
    if (b2 & 0x01) btn |= SURF_BTN_L;        /* LB */
    if (b2 & 0x02) btn |= SURF_BTN_R;        /* RB */
    if (b2 & 0x10) btn |= SURF_BTN_A;
    if (b2 & 0x20) btn |= SURF_BTN_B;
    if (b2 & 0x40) btn |= SURF_BTN_X;
    if (b2 & 0x80) btn |= SURF_BTN_Y;
    surf_pad_set_dpad(0, dpad);
    surf_pad_set_buttons(0, btn);
    /* sticks: int16 LE, scale to Q16 (x2); invert Y so stick-up is
     * negative, matching screen coordinates */
    int16_t lx = (int16_t)(r[6] | (r[7] << 8));
    int16_t ly = (int16_t)(r[8] | (r[9] << 8));
    int16_t rx = (int16_t)(r[10] | (r[11] << 8));
    int16_t ry = (int16_t)(r[12] | (r[13] << 8));
    surf_pad_set_axis(0, 0, 0, (int32_t)lx * 2);
    surf_pad_set_axis(0, 0, 1, (int32_t)(-ly) * 2);
    surf_pad_set_axis(0, 1, 0, (int32_t)rx * 2);
    surf_pad_set_axis(0, 1, 1, (int32_t)(-ry) * 2);
}

/* boot keyboard report -> events + held state on this device's slot */
static void keyboard_report(hid_dev_t *hd, const uint8_t *r)
{
    for (int i = 2; i < 8; i++) {
        uint8_t u = r[i];
        if (u < 4)
            continue;
        bool was = false;
        for (int j = 0; j < 6; j++)
            was |= hd->prev[j] == u;
        if (!was) {
            handle_usage(u, r[0]);
            hd->held_usage = u;
            hd->held_mods = r[0];
            hd->held_since_us = esp_timer_get_time();
            hd->last_rep_us = hd->held_since_us;
        }
    }
    if (hd->held_usage) {   /* release of the held key stops the repeat */
        bool still = false;
        for (int i = 2; i < 8; i++)
            still |= r[i] == hd->held_usage;
        if (!still)
            hd->held_usage = 0;
    }
    memcpy(hd->prev, r + 2, 6);
    hd->mods = r[0];
}

static void report_cb(usb_transfer_t *t)
{
    hid_dev_t *hd = t->context;
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        const uint8_t *r = t->data_buffer;
        if (hd->gamepad) {
            if (t->actual_num_bytes >= 14 && r[0] == 0x00)
                gamepad_report(r);
        } else if (t->actual_num_bytes >= 8) {
            keyboard_report(hd, r);
        }
    }
    if (hd->dev)
        usb_host_transfer_submit(t);   /* keep listening */
}

static void ctrl_done_cb(usb_transfer_t *t)
{
    usb_host_transfer_free(t);
}

/* interrupt-IN endpoint of the first interface matching (class, sub);
 * sub < 0 means "any subclass". Returns the interface, ep out-param. */
static const usb_intf_desc_t *find_iface(const usb_config_desc_t *cfg,
                                         int cls, int sub, int proto,
                                         const usb_ep_desc_t **ep_out)
{
    const usb_intf_desc_t *want = NULL, *cur = NULL;
    const usb_ep_desc_t *ep = NULL;
    int off = 0;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
    while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &off)) != NULL) {
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *i = (const usb_intf_desc_t *)d;
            bool match = i->bInterfaceClass == cls &&
                         (sub < 0 || i->bInterfaceSubClass == sub) &&
                         (proto < 0 || i->bInterfaceProtocol == proto);
            cur = match ? i : NULL;
        } else if (cur && !ep &&
                   d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t *e = (const usb_ep_desc_t *)d;
            if ((e->bEndpointAddress & 0x80) &&
                (e->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) ==
                    USB_BM_ATTRIBUTES_XFER_INT) {
                ep = e;
                want = cur;
            }
        }
    }
    *ep_out = ep;
    return ep ? want : NULL;
}

static void open_device(uint8_t addr)
{
    hid_dev_t *hd = NULL;
    for (int i = 0; i < MAX_HID_DEV; i++) {
        if (K.devs[i].dev && K.devs[i].addr == addr)
            return;                       /* already open */
        if (!hd && !K.devs[i].dev)
            hd = &K.devs[i];
    }
    if (!hd)
        return;                           /* no free slot */

    usb_device_handle_t dev;
    if (usb_host_device_open(K.client, addr, &dev) != ESP_OK)
        return;
    const usb_config_desc_t *cfg;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) {
        usb_host_device_close(K.client, dev);
        return;
    }

    /* boot keyboard (HID/1/1), else XInput gamepad (ff/5d, any proto) */
    const usb_ep_desc_t *ep = NULL;
    const usb_intf_desc_t *intf = find_iface(cfg, USB_CLASS_HID, 1, 1, &ep);
    bool gamepad = false;
    if (!intf) {
        intf = find_iface(cfg, 0xff, 0x5d, -1, &ep);
        gamepad = true;
    }
    if (!intf) {
        ESP_LOGW(TAG, "device %u: no keyboard or XInput interface", addr);
        usb_host_device_close(K.client, dev);
        return;
    }
    uint8_t iface = intf->bInterfaceNumber;
    if (usb_host_interface_claim(K.client, dev, iface, 0) != ESP_OK) {
        usb_host_device_close(K.client, dev);
        return;
    }

    if (!gamepad) {
        /* SET_PROTOCOL(boot): fire and forget */
        usb_transfer_t *ctrl;
        if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t), 0, &ctrl) == ESP_OK) {
            usb_setup_packet_t *s = (usb_setup_packet_t *)ctrl->data_buffer;
            s->bmRequestType = 0x21;
            s->bRequest = 0x0b;   /* SET_PROTOCOL */
            s->wValue = 0;        /* boot */
            s->wIndex = iface;
            s->wLength = 0;
            ctrl->num_bytes = sizeof *s;
            ctrl->device_handle = dev;
            ctrl->bEndpointAddress = 0;
            ctrl->callback = ctrl_done_cb;
            ctrl->context = NULL;
            if (usb_host_transfer_submit_control(K.client, ctrl) != ESP_OK)
                usb_host_transfer_free(ctrl);
        }
    }

    uint16_t mps = ep->wMaxPacketSize;
    usb_transfer_t *xfer;
    if (usb_host_transfer_alloc(mps < 8 ? 8 : mps, 0, &xfer) != ESP_OK) {
        usb_host_interface_release(K.client, dev, iface);
        usb_host_device_close(K.client, dev);
        return;
    }
    memset(hd, 0, sizeof *hd);
    hd->dev = dev;
    hd->addr = addr;
    hd->iface = iface;
    hd->xfer = xfer;
    hd->gamepad = gamepad;
    xfer->device_handle = dev;
    xfer->bEndpointAddress = ep->bEndpointAddress;
    xfer->num_bytes = mps;
    xfer->callback = report_cb;
    xfer->context = hd;
    xfer->timeout_ms = 0;
    if (usb_host_transfer_submit(xfer) == ESP_OK) {
        ESP_LOGI(TAG, "%s connected (addr %u, iface %u)",
                 gamepad ? "gamepad" : "keyboard", addr, iface);
        return;
    }
    usb_host_transfer_free(xfer);
    usb_host_interface_release(K.client, dev, iface);
    usb_host_device_close(K.client, dev);
    memset(hd, 0, sizeof *hd);
}

static void client_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        uint8_t nt = (uint8_t)((K.addr_tail + 1) % 8);
        if (nt != K.addr_head) {   /* drop only if the ring is full */
            K.addr_q[K.addr_tail] = msg->new_dev.address;
            K.addr_tail = nt;
        }
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        for (int i = 0; i < MAX_HID_DEV; i++) {
            hid_dev_t *hd = &K.devs[i];
            if (hd->dev != msg->dev_gone.dev_hdl)
                continue;
            usb_host_interface_release(K.client, hd->dev, hd->iface);
            usb_host_device_close(K.client, hd->dev);
            if (hd->xfer)
                usb_host_transfer_free(hd->xfer);
            if (hd->gamepad)
                surf_pad_reset(0);
            ESP_LOGI(TAG, "%s disconnected", hd->gamepad ? "gamepad" : "keyboard");
            memset(hd, 0, sizeof *hd);
            break;
        }
    }
}

static void kbd_task(void *arg)
{
    (void)arg;
    /* peripheral 1 = the P4's Full-Speed OTG, internal PHY on GPIO
     * 26/27 (LCD backlight/reset were moved to GPIO 4/5 to free them). */
    usb_host_config_t host_cfg = {.intr_flags = 0, .peripheral_map = BIT1};
    if (usb_host_install(&host_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed");
        vTaskDelete(NULL);
        return;
    }
    usb_host_client_config_t cli_cfg = {
        .max_num_event_msg = 8,
        .async = {.client_event_callback = client_cb},
    };
    if (usb_host_client_register(&cli_cfg, &K.client) != ESP_OK) {
        ESP_LOGE(TAG, "client register failed");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "USB host up (FS peripheral 1)");
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(1, &flags);
        usb_host_client_handle_events(K.client, 1);
        while (K.addr_head != K.addr_tail) {
            uint8_t a = K.addr_q[K.addr_head];
            K.addr_head = (uint8_t)((K.addr_head + 1) % 8);
            open_device(a);
        }
    }
}

void surfer_usb_kbd_init(void)
{
    if (K.q)
        return;
    K.q = xQueueCreate(64, sizeof(surfer_key));
    xTaskCreate(kbd_task, "surfer_kbd", 4096, NULL, 5, NULL);
}

/* currently-down keys, merged across all connected keyboards (state,
 * not events — this is what lets a game move AND fire) */
int surfer_usb_kbd_held(surfer_key *out, int max)
{
    int n = 0;
    for (int d = 0; d < MAX_HID_DEV && n < max; d++) {
        hid_dev_t *hd = &K.devs[d];
        if (!hd->dev || hd->gamepad)
            continue;
        for (int i = 0; i < 6 && n < max; i++)
            if (hd->prev[i] >= 4 && usage_to_key(hd->prev[i], hd->mods, &out[n]))
                n++;
    }
    return n;
}

bool surfer_usb_kbd_gamepad(void)
{
    for (int d = 0; d < MAX_HID_DEV; d++)
        if (K.devs[d].dev && K.devs[d].gamepad)
            return true;
    return false;
}

bool surfer_usb_kbd_poll(surfer_key *out)
{
    if (!K.q)
        return false;
    /* synthesize typematic repeats per keyboard (350 ms delay, ~18 Hz) */
    int64_t now = esp_timer_get_time();
    for (int d = 0; d < MAX_HID_DEV; d++) {
        hid_dev_t *hd = &K.devs[d];
        if (!hd->dev || hd->gamepad || !hd->held_usage)
            continue;
        if (now - hd->held_since_us > KBD_REPEAT_DELAY_US &&
            now - hd->last_rep_us > KBD_REPEAT_RATE_US) {
            hd->last_rep_us = now;
            handle_usage(hd->held_usage, hd->held_mods);
        }
    }
    return xQueueReceive(K.q, out, 0) == pdTRUE;
}
