#include "lcd_st7789.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "LCD_ST7789";

/* ------------------------------------------------------------------ */
/*  Static module state                                                */
/* ------------------------------------------------------------------ */
static spi_device_handle_t s_spi_dev = NULL;
static lcd_st7789_config_t s_cfg = {0};
static bool s_initialized = false;

/* ------------------------------------------------------------------ */
/*  Low‑level helpers                                                  */
/* ------------------------------------------------------------------ */
static inline void lcd_dc_low(void)
{
    gpio_set_level(s_cfg.dc_io, 0);
}

static inline void lcd_dc_high(void)
{
    gpio_set_level(s_cfg.dc_io, 1);
}

/** Send a single byte as command (DC low → CS auto → transfer → CS auto). */
static void lcd_write_cmd(uint8_t cmd)
{
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
        .user = (void *)0,  // DC-low marker not needed; we drive DC manually
    };
    lcd_dc_low();
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi_dev, &t));
}

/** Send a single byte as data. */
static void lcd_write_data(uint8_t data)
{
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &data,
        .user = (void *)1,
    };
    lcd_dc_high();
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi_dev, &t));
}

/** Send a buffer as data in one SPI transaction. */
static void lcd_write_data_buf(const uint8_t *buf, size_t len)
{
    if (len == 0) return;
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = buf,
        .user = (void *)1,
    };
    lcd_dc_high();
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
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(s_cfg.rst_io, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(s_cfg.rst_io, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

/* ------------------------------------------------------------------ */
/*  ST7789 initialisation sequence                                     */
/* ------------------------------------------------------------------ */
static void lcd_display_init(void)
{
    lcd_hw_reset();

    lcd_write_cmd(0x01);   // SWRESET
    vTaskDelay(pdMS_TO_TICKS(150));
    lcd_write_cmd(0x11);   // SLPOUT
    vTaskDelay(pdMS_TO_TICKS(150));

    lcd_write_cmd(0x3A);   // COLMOD – 16-bit / RGB565
    lcd_write_data(0x55);

    lcd_write_cmd(0x36);   // MADCTL
    lcd_write_data(0x00);

    lcd_write_cmd(0x21);   // INVON
    vTaskDelay(pdMS_TO_TICKS(10));

    lcd_write_cmd(0x13);   // NORON
    vTaskDelay(pdMS_TO_TICKS(10));
    lcd_write_cmd(0x29);   // DISPON
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

    /* --- GPIO: CS and DC --- */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << s_cfg.cs_io) | (1ULL << s_cfg.dc_io),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(s_cfg.cs_io, 1);   // CS inactive
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
        .mode = 0,                         // SPI mode 0
        .clock_speed_hz = s_cfg.spi_freq_hz,
        .spics_io_num = s_cfg.cs_io,
        .queue_size = 7,
        .flags = SPI_DEVICE_NO_DUMMY,
    };

    ESP_RETURN_ON_ERROR(
        spi_bus_add_device(SPI2_HOST, &dev_cfg, &s_spi_dev),
        TAG, "spi_bus_add_device failed");

    /* --- LCD init sequence --- */
    lcd_display_init();

    s_initialized = true;
    ESP_LOGI(TAG, "ST7789 initialized: %dx%d @ %lu Hz, pins MOSI=%d SCLK=%d CS=%d DC=%d RST=%d BL=%d",
             s_cfg.width, s_cfg.height,
             (unsigned long)s_cfg.spi_freq_hz,
             s_cfg.mosi_io, s_cfg.sclk_io, s_cfg.cs_io, s_cfg.dc_io,
             s_cfg.rst_io, s_cfg.bl_io);

    return ESP_OK;
}

void lcd_st7789_fill_screen(uint16_t color)
{
    if (!s_initialized) return;
    lcd_st7789_fill_rect(0, 0, s_cfg.width, s_cfg.height, color);
}

void lcd_st7789_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                          uint16_t color)
{
    if (!s_initialized) return;

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
    lcd_dc_high();
    while (remaining > 0) {
        uint32_t n = (remaining < LCD_CHUNK_PIXELS) ? remaining : LCD_CHUNK_PIXELS;
        spi_transaction_t t = {
            .length = n * 2 * 8,
            .tx_buffer = chunk,
        };
        ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi_dev, &t));
        remaining -= n;
    }
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
