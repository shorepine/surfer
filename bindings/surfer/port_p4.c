/* ESP32-P4 platform glue for modsurfer: EK79007 1024×600 DSI panel,
 * GT911 touch, USB-host keyboard, PSRAM asset prep. Deliberately built
 * on core-IDF APIs only (no BSP / managed components) so it drops into
 * the MicroPython esp32 port's build; the wiring constants come from the
 * ESP32-P4-Function-EV-Board BSP sources. */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "surfer_port.h"
#include "hal_p4.h"
#include "usb_kbd.h"

#define TAG "surfer_p4"

/* ESP32-P4-Function-EV-Board wiring (from the BSP) */
#define LCD_W            1024
#define LCD_H            600
#define PIN_BACKLIGHT    26
#define PIN_LCD_RST      27
#define PIN_I2C_SDA      7
#define PIN_I2C_SCL      8
#define DSI_LANES        2
#define DSI_LANE_MBPS    1000
#define DSI_LDO_CHAN     3     /* LDO_VO3 feeds VDD_MIPI_DPHY */
#define DSI_LDO_MV       2500
#define GT911_ADDR       0x5D
#define GT911_ADDR_ALT   0x14
#define GT911_REG_STATUS 0x814E
#define GT911_REG_POINT  0x8150

static struct {
    esp_lcd_panel_handle_t panel;
    i2c_master_dev_handle_t touch;
} P;

/* ---- EK79007 init: PAD_CONTROL for 2-lane, vendor registers, sleep-out
 * (sequence from espressif/esp_lcd_ek79007, Apache-2.0) ---- */
static esp_err_t ek79007_init_cmds(esp_lcd_panel_io_handle_t io)
{
    static const struct { uint8_t cmd, val; } regs[] = {
        {0xB2, 0x10},  /* PAD_CONTROL: 2-lane DSI */
        {0x80, 0x8B}, {0x81, 0x78}, {0x82, 0x84}, {0x83, 0x88},
        {0x84, 0xA8}, {0x85, 0xE3}, {0x86, 0x88},
    };
    for (size_t i = 0; i < sizeof regs / sizeof regs[0]; i++)
        ESP_RETURN_ON_ERROR(
            esp_lcd_panel_io_tx_param(io, regs[i].cmd, &regs[i].val, 1), TAG,
            "ek79007 reg");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x11, NULL, 0), TAG,
                        "sleep out");
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

static esp_err_t display_init(void)
{
    /* DSI PHY power */
    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = DSI_LDO_CHAN,
        .voltage_mv = DSI_LDO_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo), TAG, "ldo");

    esp_lcd_dsi_bus_handle_t bus = NULL;
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = DSI_LANES,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = DSI_LANE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &bus), TAG, "dsi bus");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(bus, &dbi_cfg, &io), TAG, "dbi");

    /* hardware reset, active low */
    gpio_config_t rst = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_LCD_RST,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rst), TAG, "rst gpio");
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* DPI panel: EK79007 1024x600 60Hz timing, 3 fbs for the triple-buffer
     * presentation (DESIGN.md §5.2) */
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 52,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = 3,
        .video_timing = {
            .h_size = LCD_W,
            .v_size = LCD_H,
            .hsync_pulse_width = 10,
            .hsync_back_porch = 160,
            .hsync_front_porch = 160,
            .vsync_pulse_width = 1,
            .vsync_back_porch = 23,
            .vsync_front_porch = 12,
        },
        .flags = {.use_dma2d = true},
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_dpi(bus, &dpi_cfg, &P.panel), TAG, "dpi");

    ESP_RETURN_ON_ERROR(ek79007_init_cmds(io), TAG, "panel cmds");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(P.panel), TAG, "panel init");

    gpio_config_t bl = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_BACKLIGHT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bl), TAG, "backlight gpio");
    gpio_set_level(PIN_BACKLIGHT, 1);
    return ESP_OK;
}

/* ---- GT911, minimal register reads (16-bit big-endian addressing) ---- */

static esp_err_t gt911_read(uint16_t reg, uint8_t *buf, size_t len)
{
    uint8_t addr[2] = {(uint8_t)(reg >> 8), (uint8_t)reg};
    return i2c_master_transmit_receive(P.touch, addr, 2, buf, len, 50);
}

static esp_err_t gt911_write0(uint16_t reg)
{
    uint8_t frame[3] = {(uint8_t)(reg >> 8), (uint8_t)reg, 0};
    return i2c_master_transmit(P.touch, frame, 3, 50);
}

static esp_err_t touch_init(void)
{
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 1,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "i2c bus");

    const uint16_t addrs[] = {GT911_ADDR, GT911_ADDR_ALT};
    for (size_t i = 0; i < 2; i++) {
        if (i2c_master_probe(bus, addrs[i], 100) == ESP_OK) {
            i2c_device_config_t dev_cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = addrs[i],
                .scl_speed_hz = 400000,
            };
            ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &P.touch),
                                TAG, "gt911 dev");
            ESP_LOGI(TAG, "GT911 at 0x%02x", addrs[i]);
            return ESP_OK;
        }
    }
    ESP_LOGW(TAG, "GT911 not found — touch disabled");
    return ESP_ERR_NOT_FOUND;
}

static bool touch_poll(int16_t *x, int16_t *y)
{
    if (!P.touch)
        return false;
    uint8_t status = 0;
    if (gt911_read(GT911_REG_STATUS, &status, 1) != ESP_OK)
        return false;
    if (!(status & 0x80))
        return false;  /* no fresh data */
    uint8_t n = status & 0x0f;
    bool down = false;
    if (n > 0) {
        uint8_t p[6];
        if (gt911_read(GT911_REG_POINT, p, sizeof p) == ESP_OK) {
            /* the panel is mounted 180° from the touch controller: the
             * BSP mirrors both axes (mirror_x = mirror_y = 1) and so
             * must we */
            int16_t rx = (int16_t)(p[0] | (p[1] << 8));
            int16_t ry = (int16_t)(p[2] | (p[3] << 8));
            if (rx < 0) rx = 0;
            if (rx >= LCD_W) rx = LCD_W - 1;
            if (ry < 0) ry = 0;
            if (ry >= LCD_H) ry = LCD_H - 1;
            *x = (int16_t)(LCD_W - 1 - rx);
            *y = (int16_t)(LCD_H - 1 - ry);
            down = true;
        }
    }
    gt911_write0(GT911_REG_STATUS);  /* ack */
    return down;
}

/* ---- surfer_port ---- */

const surf_hal *surfer_port_init(int16_t w, int16_t h, bool single_buffer)
{
    (void)w; (void)h;  /* the panel is what it is */
    if (display_init() != ESP_OK)
        return NULL;
    touch_init();  /* soft-fail: keyboard-only still works */
    usb_kbd_init();

    void *fb0 = NULL, *fb1 = NULL, *fb2 = NULL;
    if (esp_lcd_dpi_panel_get_frame_buffer(P.panel, 3, &fb0, &fb1, &fb2) != ESP_OK)
        return NULL;

    surf_hal_p4_cfg cfg = {
        .panel = P.panel,
        .scan_fbs = {fb0, fb1, fb2},
        .w = LCD_W,
        .h = LCD_H,
        .touch_poll = touch_poll,
        .single_buffer = single_buffer,
    };
    return surf_hal_p4_init(&cfg);
}

bool surfer_port_pump(void)
{
    /* Present never blocks (triple buffering), so pace the Python loop
     * here — a short sleep keeps FreeRTOS breathing without eating a
     * third of a 60fps frame budget. */
    vTaskDelay(pdMS_TO_TICKS(2));
    return true;
}

bool surfer_port_poll_key(surfer_key *out)
{
    return usb_kbd_poll(out);
}

bool surfer_port_screenshot(const char *path)
{
    /* No C-side path: MicroPython's VFS is not IDF's. Use
     * surfer.fb_read() from Python and write with Python file IO. */
    (void)path;
    return false;
}

void surfer_port_fb_sync_for_read(void)
{
    surf_hal_p4_fb_invalidate();
}

void surfer_port_prepare_image(surf_image *img)
{
    /* flash .rodata → PSRAM: the PPA cannot DMA from memory-mapped flash */
    size_t bytes = (size_t)img->stride * img->h;
    size_t sz = (bytes + 127) & ~(size_t)127;
    void *px = heap_caps_aligned_alloc(128, sz, MALLOC_CAP_SPIRAM);
    if (!px) {
        ESP_LOGE(TAG, "asset PSRAM alloc failed (%u bytes)", (unsigned)bytes);
        return;
    }
    memcpy(px, img->pixels, bytes);
    esp_cache_msync(px, sz, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    img->pixels = px;
}
