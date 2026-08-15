#include "lcd_st7789.h"

#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lcd_font.h"
#include "rom/ets_sys.h"   /* ets_delay_us: busy wait, 不让出 CPU（L5 sleep_in/out 持锁期间用）*/

static const char *TAG = "LCD_ST7789";

/* ------------------------------------------------------------------ */
/*  Static module state                                                */
/* ------------------------------------------------------------------ */
static spi_device_handle_t s_spi_dev = NULL;
static lcd_st7789_config_t s_cfg = {0};
static bool s_initialized = false;
static volatile bool s_sleeping = false;    /* L5: ST7789 Sleep 状态跟踪 */
/* 递归互斥锁：保护 SPI 操作序列，防止 bl_task 和 sr_detect 并发访问 SPI 崩溃。
 * 递归锁：lcd_wake_if_sleeping() 调 sleep_out()，而 sleep_out 在 fill_rect 持锁期间被调，
 * 递归锁允许同任务多次获取不死锁。*/
static SemaphoreHandle_t s_lcd_lock = NULL;

static inline void lcd_lock(void)   { xSemaphoreTakeRecursive(s_lcd_lock, portMAX_DELAY); }
static inline void lcd_unlock(void) { xSemaphoreGiveRecursive(s_lcd_lock); }

/* ================================================================== */
/*  Pre‑transaction callback: set DC pin according to `user` field     */
/*  (0 = command, 1 = data).  This is the ESP‑IDF recommended way.    */
/* ================================================================== */
static void lcd_spi_pre_cb(spi_transaction_t *t)
{
    int dc_level = (int)(uintptr_t)t->user;
    gpio_set_level(s_cfg.dc_io, dc_level);
}

/* ------------------------------------------------------------------ */
/*  Low‑level helpers                                                  */
/* ------------------------------------------------------------------ */

/** Send a single byte as command. */
static void lcd_write_cmd(uint8_t cmd)
{
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
        .user = (void *)0,   /* DC low = command */
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi_dev, &t));
}

/** Send a single byte as data. */
static void lcd_write_data(uint8_t data)
{
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &data,
        .user = (void *)1,   /* DC high = data */
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi_dev, &t));
}

/** Send a buffer as data in one SPI transaction. */
static void lcd_write_data_buf(const uint8_t *buf, size_t len)
{
    if (len == 0) return;
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = buf,
        .user = (void *)1,   /* DC high = data */
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi_dev, &t));
}

/** Set the column + row address window and prepare for RAM write. */
static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t buf[4];

    lcd_write_cmd(0x2A);  // CASET
    buf[0] = x0 >> 8;
    buf[1] = x0 & 0xFF;
    buf[2] = x1 >> 8;
    buf[3] = x1 & 0xFF;
    lcd_write_data_buf(buf, sizeof(buf));

    lcd_write_cmd(0x2B);  // RASET
    buf[0] = y0 >> 8;
    buf[1] = y0 & 0xFF;
    buf[2] = y1 >> 8;
    buf[3] = y1 & 0xFF;
    lcd_write_data_buf(buf, sizeof(buf));

    lcd_write_cmd(0x2C);  // RAMWR
}

/* ------------------------------------------------------------------ */
/*  Hardware reset                                                     */
/* ------------------------------------------------------------------ */
static void lcd_hw_reset(void)
{
    if (s_cfg.rst_io < 0) return;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << s_cfg.rst_io),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    gpio_set_level(s_cfg.rst_io, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(s_cfg.rst_io, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(s_cfg.rst_io, 1);
    vTaskDelay(pdMS_TO_TICKS(150));   /* wait for display to stabilise */
}

/* ------------------------------------------------------------------ */
/*  ST7789 initialisation sequence                                     */
/* ------------------------------------------------------------------ */
static void lcd_display_init(void)
{
    lcd_hw_reset();

    lcd_write_cmd(0x01);   /* SWRESET */
    vTaskDelay(pdMS_TO_TICKS(150));

    lcd_write_cmd(0x11);   /* SLPOUT – exit sleep */
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_write_cmd(0x3A);   /* COLMOD – pixel format */
    lcd_write_data(0x55);  /* 16‑bit / RGB565 */

    lcd_write_cmd(0xB2);   /* PORCTRL – porch control */
    lcd_write_data(0x0C);
    lcd_write_data(0x0C);
    lcd_write_data(0x00);
    lcd_write_data(0x33);
    lcd_write_data(0x33);

    lcd_write_cmd(0xB7);   /* GCTRL – gate control */
    lcd_write_data(0x35);

    lcd_write_cmd(0xBB);   /* VCOMS – VCOM setting */
    lcd_write_data(0x19);

    lcd_write_cmd(0xC0);   /* LCMCTRL – LCM control */
    lcd_write_data(0x2C);

    lcd_write_cmd(0xC2);   /* VDVVRHEN – VDV and VRH enable */
    lcd_write_data(0x01);

    lcd_write_cmd(0xC3);   /* VRHS – VRH set */
    lcd_write_data(0x12);

    lcd_write_cmd(0xC4);   /* VDVS – VDV set */
    lcd_write_data(0x20);

    lcd_write_cmd(0xC6);   /* FRCTRL2 – frame rate control */
    lcd_write_data(0x0F);

    lcd_write_cmd(0xD0);   /* PWCTRL1 – power control 1 */
    lcd_write_data(0xA4);
    lcd_write_data(0xA1);

    lcd_write_cmd(0xE0);   /* PVGAMCTRL – positive gamma */
    lcd_write_data(0xD0);
    lcd_write_data(0x04);
    lcd_write_data(0x0D);
    lcd_write_data(0x11);
    lcd_write_data(0x13);
    lcd_write_data(0x2B);
    lcd_write_data(0x3F);
    lcd_write_data(0x54);
    lcd_write_data(0x4C);
    lcd_write_data(0x18);
    lcd_write_data(0x0D);
    lcd_write_data(0x0B);
    lcd_write_data(0x1F);
    lcd_write_data(0x23);

    lcd_write_cmd(0xE1);   /* NVGAMCTRL – negative gamma */
    lcd_write_data(0xD0);
    lcd_write_data(0x04);
    lcd_write_data(0x0C);
    lcd_write_data(0x11);
    lcd_write_data(0x13);
    lcd_write_data(0x2C);
    lcd_write_data(0x3F);
    lcd_write_data(0x44);
    lcd_write_data(0x51);
    lcd_write_data(0x2F);
    lcd_write_data(0x1F);
    lcd_write_data(0x1F);
    lcd_write_data(0x20);
    lcd_write_data(0x23);

    lcd_write_cmd(0x36);   /* MADCTL – memory data access control */
    lcd_write_data(0x00);

    lcd_write_cmd(0x21);   /* INVON – display inversion on */
    vTaskDelay(pdMS_TO_TICKS(10));

    lcd_write_cmd(0x13);   /* NORON – normal display mode */
    vTaskDelay(pdMS_TO_TICKS(10));

    lcd_write_cmd(0x29);   /* DISPON – display on */
    vTaskDelay(pdMS_TO_TICKS(100));
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */
esp_err_t lcd_st7789_init(const lcd_st7789_config_t *cfg)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    s_cfg = *cfg;

    /* 创建递归互斥锁（在第一次 SPI 操作之前）*/
    s_lcd_lock = xSemaphoreCreateRecursiveMutex();
    if (s_lcd_lock == NULL) {
        ESP_LOGE(TAG, "Failed to create LCD lock");
        return ESP_ERR_NO_MEM;
    }

    /* --- GPIO: DC only (CS is managed by SPI driver) --- */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << s_cfg.dc_io),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(s_cfg.dc_io, 1);   // default high

    /* --- Backlight --- */
    if (s_cfg.bl_io >= 0) {
        gpio_config_t bl_conf = {
            .pin_bit_mask = (1ULL << s_cfg.bl_io),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&bl_conf);
        gpio_set_level(s_cfg.bl_io, 1);
    }

    /* --- SPI bus init (host = SPI2_HOST for GPSPI2) --- */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = s_cfg.mosi_io,
        .miso_io_num = -1,
        .sclk_io_num = s_cfg.sclk_io,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = s_cfg.width * s_cfg.height * 2 + 8,
    };

    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
        TAG, "spi_bus_initialize failed");

    /* --- Add ST7789 device --- */
    spi_device_interface_config_t dev_cfg = {
        .mode = 0,                         /* SPI mode 0 */
        .clock_speed_hz = s_cfg.spi_freq_hz,
        .spics_io_num = s_cfg.cs_io,       /* driver controls CS */
        .queue_size = 7,
        .flags = SPI_DEVICE_NO_DUMMY,
        .pre_cb = lcd_spi_pre_cb,          /* DC pin per‑transaction */
    };

    ESP_RETURN_ON_ERROR(
        spi_bus_add_device(SPI2_HOST, &dev_cfg, &s_spi_dev),
        TAG, "spi_bus_add_device failed");

    /* --- LCD init sequence --- */
    lcd_display_init();

    /* --- Quick diagnostic: fill red → black so user sees SPI works --- */
    ESP_LOGI(TAG, "Diagnostic: filling screen red...");
    lcd_st7789_fill_rect(0, 0, s_cfg.width, s_cfg.height, 0xF800);
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_LOGI(TAG, "Diagnostic: filling screen black...");
    lcd_st7789_fill_rect(0, 0, s_cfg.width, s_cfg.height, 0x0000);

    s_initialized = true;
    ESP_LOGI(TAG, "ST7789 initialized: %dx%d @ %lu Hz, pins MOSI=%d SCLK=%d CS=%d DC=%d RST=%d BL=%d",
             s_cfg.width, s_cfg.height,
             (unsigned long)s_cfg.spi_freq_hz,
             s_cfg.mosi_io, s_cfg.sclk_io, s_cfg.cs_io, s_cfg.dc_io,
             s_cfg.rst_io, s_cfg.bl_io);

    return ESP_OK;
}

/* ==================================================================
 * L5: Sleep / Wake-up ST7789 驱动振荡器与扫描
 * ================================================================== */
void lcd_st7789_sleep_in(void)
{
    if (!s_initialized) return;
    lcd_lock();
    if (s_sleeping) { lcd_unlock(); return; }  /* 防重复调用 */
    /* ⚠️ 先设 s_sleeping=true 再发 SPI 命令：
     * 防止 delay 期间 sr_detect 误唤醒 → lcd_ui_update → 并发访问 SPI 崩溃。*/
    s_sleeping = true;
    ESP_LOGI(TAG, "[SENTRY-BEGIN] L5: ST7789 SLEEP_IN (0x10) — send cmd");
    lcd_write_cmd(0x10);  /* SLPIN */
    /* ST7789 数据表 t(SLPIN) = 5ms。用 ets_delay_us（busy wait）而非 vTaskDelay：
     * 持锁期间不让出 CPU，避免高优先级任务抢锁后并发访问 SPI。*/
    ets_delay_us(5000);
    lcd_unlock();
    ESP_LOGI(TAG, "[SENTRY-OK] L5: ST7789 SLEEP_IN done");
}

void lcd_st7789_sleep_out(void)
{
    if (!s_initialized) return;
    lcd_lock();
    if (!s_sleeping) { lcd_unlock(); return; }
    ESP_LOGI(TAG, "[SENTRY-BEGIN] L5: ST7789 SLEEP_OUT (0x11) — send cmd + busy 120ms");
    lcd_write_cmd(0x11);  /* SLPOUT */
    /* ST7789 数据表 t(SLPOUT) ≥ 120ms。用 ets_delay_us（busy wait）而非 vTaskDelay：
     * 持锁期间不让出 CPU，确保 120ms 内没有其他任务访问 SPI。*/
    ets_delay_us(120000);
    s_sleeping = false;
    lcd_unlock();
    ESP_LOGI(TAG, "[SENTRY-OK] L5: ST7789 SLEEP_OUT done");
}

bool lcd_st7789_is_sleeping(void) { return s_sleeping; }

/* ==================================================================
 * L5: 所有 draw/fill 入口加检查：如处于 Sleep 先自动唤醒再绘制。
 *     避免 lcd_ui_update() 在 sleep 期间画画导致写入被丢弃。
 * ================================================================== */
static inline void lcd_wake_if_sleeping(void)
{
    if (s_sleeping) {
        lcd_st7789_sleep_out();
    }
}

void lcd_st7789_fill_screen(uint16_t color)
{
    if (!s_initialized) return;
    lcd_lock();
    lcd_wake_if_sleeping();
    lcd_st7789_fill_rect(0, 0, s_cfg.width, s_cfg.height, color);
    lcd_unlock();
}

void lcd_st7789_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                          uint16_t color)
{
    if (!s_initialized) return;
    lcd_lock();
    lcd_wake_if_sleeping();

    lcd_set_window(x, y, x + w - 1, y + h - 1);

    const uint8_t hi = color >> 8;
    const uint8_t lo = color & 0xFF;
    const uint32_t total = (uint32_t)w * h;

    /* Send pixel data in chunks to keep stack usage low */
    #define LCD_CHUNK_PIXELS 256
    static uint8_t chunk[LCD_CHUNK_PIXELS * 2];

    /* Pre-fill one chunk */
    const uint32_t fill_pixels = (total < LCD_CHUNK_PIXELS) ? total : LCD_CHUNK_PIXELS;
    for (uint32_t i = 0; i < fill_pixels; i++) {
        chunk[i * 2]     = hi;
        chunk[i * 2 + 1] = lo;
    }

    uint32_t remaining = total;
    while (remaining > 0) {
        uint32_t n = (remaining < LCD_CHUNK_PIXELS) ? remaining : LCD_CHUNK_PIXELS;
        spi_transaction_t t = {
            .length = n * 2 * 8,
            .tx_buffer = chunk,
            .user = (void *)1,    /* ← DC high: pixel data */
        };
        ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi_dev, &t));
        remaining -= n;
    }
    lcd_unlock();
}

/* ------------------------------------------------------------------ */
/*  Pixel drawing                                                      */
/* ------------------------------------------------------------------ */
void lcd_st7789_draw_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    if (!s_initialized) return;
    lcd_lock();
    lcd_wake_if_sleeping();
    lcd_set_window(x, y, x, y);
    uint8_t buf[2] = { color >> 8, color & 0xFF };
    /* lcd_write_data_buf sets DC via .user=1, no manual DC needed */
    lcd_write_data_buf(buf, 2);
    lcd_unlock();
}

/* ------------------------------------------------------------------ */
/*  Text rendering                                                     */
/* ------------------------------------------------------------------ */

/**
 * Draw one glyph at (x, y).  The caller has already set DC appropriately;
 * we set the window and send pixel rows.
 *
 * Each row of the glyph is drawn as one or two contiguous pixel segments
 * (left half + right half for 16‑px‑wide glyphs).  We build a temporary
 * line buffer and transmit it.
 */
static void draw_glyph(uint16_t x, uint16_t y,
                       const uint8_t *bitmap, uint8_t w,
                       uint16_t fg, uint16_t bg)
{
    if (!bitmap || w == 0) return;

    uint8_t line_buf[32];           /* 最多 16 px × 2 bytes = 32 bytes */
    uint8_t bytes_per_row = w / 8;  /* ASCII=1, 中文=2 */

    for (uint8_t row = 0; row < LCD_FONT_H; row++) {
        uint8_t *dst = line_buf;
        for (uint8_t col = 0; col < w; col++) {
            uint8_t byte_idx = col / 8;
            uint8_t bit_idx  = col % 8;
            bool on = (bitmap[row * bytes_per_row + byte_idx] >> (7 - bit_idx)) & 1;
            uint16_t c = on ? fg : bg;
            *dst++ = c >> 8;
            *dst++ = c & 0xFF;
        }
        lcd_set_window(x, y + row, x + w - 1, y + row);
        spi_transaction_t t = {
            .length = w * 2 * 8,
            .tx_buffer = line_buf,
            .user = (void *)1,    /* DC high */
        };
        ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi_dev, &t));
    }
}

/* ------------------------------------------------------------------
 *  UTF-8 decode helper — returns Unicode codepoint, sets *consumed.
 *  Invalid sequence returns 0xFFFFFFFF and consumes 1 byte.
 * ------------------------------------------------------------------ */
static uint32_t utf8_decode(const char *p, int *consumed)
{
    uint8_t c = (uint8_t)*p;
    if (c < 0x80) {
        *consumed = 1;
        return c;
    }
    uint32_t unicode = 0;
    int len = 0;
    if ((c & 0xE0) == 0xC0)      { len = 2; unicode = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; unicode = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; unicode = c & 0x07; }
    else { *consumed = 1; return 0xFFFFFFFF; }

    for (int i = 1; i < len; i++) {
        if (((uint8_t)p[i] & 0xC0) != 0x80) {
            *consumed = 1;
            return 0xFFFFFFFF;
        }
        unicode = (unicode << 6) | ((uint8_t)p[i] & 0x3F);
    }
    *consumed = len;
    return unicode;
}

void lcd_st7789_draw_string(uint16_t x, uint16_t y, const char *str,
                            uint16_t fg_color, uint16_t bg_color)
{
    if (!s_initialized || !str) return;
    lcd_lock();
    lcd_wake_if_sleeping();

    uint16_t cur_x = x;
    const char *p = str;
    while (*p) {
        int consumed = 0;
        uint32_t cp = utf8_decode(p, &consumed);
        uint8_t gw = 0;
        const uint8_t *glyph = NULL;

        if (cp < 0x80) {
            glyph = lcd_font_get_glyph((char)cp, &gw);
        } else if (cp != 0xFFFFFFFF) {
            glyph = lcd_font_get_glyph_cn(cp, &gw);
        }

        if (glyph && gw > 0) {
            draw_glyph(cur_x, y, glyph, gw, fg_color, bg_color);
            cur_x += gw;
        }
        p += consumed;
    }
    lcd_unlock();
}

uint16_t lcd_st7789_string_width(const char *str)
{
    if (!str) return 0;
    uint16_t w = 0;
    const char *p = str;
    while (*p) {
        int consumed = 0;
        uint32_t cp = utf8_decode(p, &consumed);
        uint8_t gw = 0;
        if (cp < 0x80) {
            lcd_font_get_glyph((char)cp, &gw);
        } else if (cp != 0xFFFFFFFF) {
            lcd_font_get_glyph_cn(cp, &gw);
        }
        w += gw;
        p += consumed;
    }
    return w;
}

void lcd_st7789_deinit(void)
{
    if (!s_initialized) return;

    if (s_spi_dev) {
        spi_bus_remove_device(s_spi_dev);
        s_spi_dev = NULL;
    }
    spi_bus_free(SPI2_HOST);
    s_initialized = false;
    ESP_LOGI(TAG, "Deinitialized");
}
