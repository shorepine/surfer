/* USB-host HID keyboard, boot protocol only, raw usb_host API — no
 * managed components so it builds inside the MicroPython esp32 port.
 * One task drives both the host library and our client; a connected
 * keyboard's interrupt IN endpoint feeds 8-byte boot reports that get
 * diffed into key events for surfer_port_poll_key. */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "usb/usb_host.h"

#include "usb_kbd.h"

#define TAG "surfer_kbd"

static struct {
    usb_host_client_handle_t client;
    usb_device_handle_t      dev;
    uint8_t                  iface;
    usb_transfer_t          *xfer;
    QueueHandle_t            q;      /* surfer_key */
    uint8_t                  prev[6];
    uint8_t                  addr_pending;
    /* software key repeat (HID boot reports carry state, not repeats) */
    uint8_t                  held_usage;
    uint8_t                  held_mods;
    int64_t                  held_since_us;
    int64_t                  last_rep_us;
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

static void emit(uint8_t kind, bool shift, char ch)
{
    surfer_key k = {.kind = kind, .shift = shift};
    if (ch) {
        k.utf8[0] = ch;
        k.utf8[1] = 0;
    }
    xQueueSend(K.q, &k, 0);
}

static void handle_usage(uint8_t u, uint8_t mods)
{
    bool shift = (mods & 0x22) != 0;
    switch (u) {
    case 0x28: emit(SURFER_KEY_ENTER, shift, 0); return;
    case 0x2a: emit(SURFER_KEY_BACKSPACE, shift, 0); return;
    case 0x2b: emit(SURFER_KEY_TEXT, false, ' '); return;  /* tab → space-ish */
    case 0x4a: emit(SURFER_KEY_HOME, shift, 0); return;
    case 0x4b: emit(SURFER_KEY_PGUP, shift, 0); return;
    case 0x4c: emit(SURFER_KEY_DELETE, shift, 0); return;
    case 0x4d: emit(SURFER_KEY_END, shift, 0); return;
    case 0x4e: emit(SURFER_KEY_PGDN, shift, 0); return;
    case 0x4f: emit(SURFER_KEY_RIGHT, shift, 0); return;
    case 0x50: emit(SURFER_KEY_LEFT, shift, 0); return;
    case 0x51: emit(SURFER_KEY_DOWN, shift, 0); return;
    case 0x52: emit(SURFER_KEY_UP, shift, 0); return;
    }
    if (u < sizeof base_map) {
        char ch = shift ? shift_map[u] : base_map[u];
        if (ch)
            emit(SURFER_KEY_TEXT, shift, ch);
    }
}

static void report_cb(usb_transfer_t *t)
{
    if (t->status == USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes >= 8) {
        const uint8_t *r = t->data_buffer;  /* [mods, 0, k1..k6] */
        for (int i = 2; i < 8; i++) {
            uint8_t u = r[i];
            if (u < 4)
                continue;
            bool was = false;
            for (int j = 0; j < 6; j++)
                was |= K.prev[j] == u;
            if (!was) {
                handle_usage(u, r[0]);
                K.held_usage = u;
                K.held_mods = r[0];
                K.held_since_us = esp_timer_get_time();
                K.last_rep_us = K.held_since_us;
            }
        }
        /* release of the held key stops the repeat */
        if (K.held_usage) {
            bool still = false;
            for (int i = 2; i < 8; i++)
                still |= r[i] == K.held_usage;
            if (!still)
                K.held_usage = 0;
        }
        memcpy(K.prev, r + 2, 6);
    }
    if (K.dev)
        usb_host_transfer_submit(t);  /* keep listening */
}

static void ctrl_done_cb(usb_transfer_t *t)
{
    usb_host_transfer_free(t);
}

static void open_device(uint8_t addr)
{
    if (K.dev)
        return;
    if (usb_host_device_open(K.client, addr, &K.dev) != ESP_OK)
        return;

    const usb_config_desc_t *cfg;
    if (usb_host_get_active_config_descriptor(K.dev, &cfg) != ESP_OK)
        goto fail;

    /* find a boot-protocol keyboard interface + its interrupt IN ep */
    const usb_intf_desc_t *kbd = NULL;
    const usb_ep_desc_t *ep = NULL;
    int off = 0;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
    while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &off)) != NULL) {
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *i = (const usb_intf_desc_t *)d;
            if (!kbd && i->bInterfaceClass == USB_CLASS_HID &&
                i->bInterfaceSubClass == 1 && i->bInterfaceProtocol == 1)
                kbd = i;
        } else if (kbd && !ep && d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t *e = (const usb_ep_desc_t *)d;
            if ((e->bEndpointAddress & 0x80) &&
                (e->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) ==
                    USB_BM_ATTRIBUTES_XFER_INT)
                ep = e;
        }
    }
    if (!kbd || !ep) {
        ESP_LOGW(TAG, "no boot keyboard interface on device %u", addr);
        goto fail;
    }
    K.iface = kbd->bInterfaceNumber;
    if (usb_host_interface_claim(K.client, K.dev, K.iface, 0) != ESP_OK)
        goto fail;

    /* SET_PROTOCOL(boot): fire and forget */
    usb_transfer_t *ctrl;
    if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t), 0, &ctrl) == ESP_OK) {
        usb_setup_packet_t *s = (usb_setup_packet_t *)ctrl->data_buffer;
        s->bmRequestType = 0x21;
        s->bRequest = 0x0b;  /* SET_PROTOCOL */
        s->wValue = 0;       /* boot */
        s->wIndex = K.iface;
        s->wLength = 0;
        ctrl->num_bytes = sizeof *s;
        ctrl->device_handle = K.dev;
        ctrl->bEndpointAddress = 0;
        ctrl->callback = ctrl_done_cb;
        ctrl->context = NULL;
        if (usb_host_transfer_submit_control(K.client, ctrl) != ESP_OK)
            usb_host_transfer_free(ctrl);
    }

    uint16_t mps = ep->wMaxPacketSize;
    if (usb_host_transfer_alloc(mps < 8 ? 8 : mps, 0, &K.xfer) != ESP_OK)
        goto fail;
    K.xfer->device_handle = K.dev;
    K.xfer->bEndpointAddress = ep->bEndpointAddress;
    K.xfer->num_bytes = mps;
    K.xfer->callback = report_cb;
    K.xfer->timeout_ms = 0;
    memset(K.prev, 0, sizeof K.prev);
    if (usb_host_transfer_submit(K.xfer) == ESP_OK) {
        ESP_LOGI(TAG, "keyboard connected (addr %u, iface %u)", addr, K.iface);
        return;
    }

fail:
    if (K.xfer) {
        usb_host_transfer_free(K.xfer);
        K.xfer = NULL;
    }
    if (K.dev) {
        usb_host_device_close(K.client, K.dev);
        K.dev = NULL;
    }
}

static void client_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        K.addr_pending = msg->new_dev.address;
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        if (K.dev == msg->dev_gone.dev_hdl) {
            usb_host_interface_release(K.client, K.dev, K.iface);
            usb_host_device_close(K.client, K.dev);
            K.dev = NULL;
            if (K.xfer) {
                usb_host_transfer_free(K.xfer);
                K.xfer = NULL;
            }
            ESP_LOGI(TAG, "keyboard disconnected");
        }
    }
}

static void kbd_task(void *arg)
{
    (void)arg;
    usb_host_config_t host_cfg = {.intr_flags = 0};
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
    ESP_LOGI(TAG, "USB host up; plug in a keyboard");
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(1, &flags);
        usb_host_client_handle_events(K.client, 1);
        if (K.addr_pending) {
            uint8_t a = K.addr_pending;
            K.addr_pending = 0;
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

bool surfer_usb_kbd_poll(surfer_key *out)
{
    if (!K.q)
        return false;
    /* synthesize repeats for the most recent held key (typematic feel:
     * 350 ms delay, then ~18 Hz) */
    if (K.held_usage && K.dev) {
        int64_t now = esp_timer_get_time();
        if (now - K.held_since_us > KBD_REPEAT_DELAY_US &&
            now - K.last_rep_us > KBD_REPEAT_RATE_US) {
            K.last_rep_us = now;
            handle_usage(K.held_usage, K.held_mods);
        }
    }
    return xQueueReceive(K.q, out, 0) == pdTRUE;
}
