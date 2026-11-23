/*
 * Driver for the 4.2 inch (400x300) e-Paper panel with an SSD1683 controller
 * (Waveshare "4.2inch e-Paper V2"). Only this controller is supported.
 *
 * Wiring (see GPIO.md):
 *   ESP32-C3 GPIO4  -> CLK
 *   ESP32-C3 GPIO6  -> DIN (MOSI)
 *   ESP32-C3 GPIO7  -> CS
 *   ESP32-C3 GPIO1  -> DC
 *   ESP32-C3 GPIO2  -> RST
 *   ESP32-C3 GPIO10 -> BUSY
 */

#include "epd4in2.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define EPD_PIN_SCLK 4
#define EPD_PIN_MOSI 6
#define EPD_PIN_CS   7
#define EPD_PIN_DC   1
#define EPD_PIN_RST  2
#define EPD_PIN_BUSY 10

#define EPD_SPI_HOST        SPI2_HOST
#define EPD_SPI_FREQ_HZ     (4 * 1000 * 1000)

/* Refresh durations of a 4.2" SSD1683 panel. BUSY cannot be used as a
 * handshake on this build: during a full update it stays high (the wire can
 * float), while a partial update releases it after ~410 ms. Use fixed,
 * generous delays instead. The real waveform lasts about 1 s (three or four
 * flashes); 2.5 s leaves margin without making the web page feel slow. A
 * waveform cut short shows its inverted intermediate image and leaves the
 * panel grey, so do not shorten this below the measured duration. */
#define EPD_REFRESH_MS      2500
#define EPD_PARTIAL_MS      3000

static const char *TAG = "epd4in2";

static spi_device_handle_t s_spi;
static uint8_t s_fb[EPD_BUF_SIZE];

/* Copy of the frame that is currently on the panel. It is only used to report
 * how many bytes a partial update really changes; the controller keeps its own
 * 0x26 previous-image plane. */
static uint8_t s_prev[EPD_BUF_SIZE];

/* ------------------------------------------------------------------------- */
/* Low level bus helpers                                                     */
/* ------------------------------------------------------------------------- */

static esp_err_t epd_send(const uint8_t *data, size_t len, bool is_cmd)
{
    gpio_set_level(EPD_PIN_DC, is_cmd ? 0 : 1);

    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    return spi_device_polling_transmit(s_spi, &t);
}

static esp_err_t epd_cmd(uint8_t cmd)
{
    return epd_send(&cmd, 1, true);
}

static esp_err_t epd_data(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return ESP_OK;
    }
    return epd_send(data, len, false);
}

static esp_err_t epd_data1(uint8_t value)
{
    return epd_data(&value, 1);
}

/* Reset pulse used by every activation. Same timing as the Waveshare reference
 * driver for this module: 100 ms high, 2 ms low, 100 ms high. A hardware reset
 * is what wakes the controller up again. */
static void epd_hw_reset(void)
{
    gpio_set_level(EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(EPD_PIN_RST, 0);
    vTaskDelay(1); /* one FreeRTOS tick = 10 ms (2 ms would round to zero) */
    gpio_set_level(EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
}

static esp_err_t epd_bus_init(void)
{
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << EPD_PIN_DC) | (1ULL << EPD_PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out_cfg), TAG, "DC/RST gpio config");

    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << EPD_PIN_BUSY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&in_cfg), TAG, "BUSY gpio config");

    gpio_set_level(EPD_PIN_DC, 1);
    gpio_set_level(EPD_PIN_RST, 1);

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = EPD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = EPD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_BUF_SIZE,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(EPD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "spi bus init");

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = EPD_SPI_FREQ_HZ,
        .mode = 0,                    /* CPOL=0, CPHA=0 */
        .spics_io_num = EPD_PIN_CS,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(EPD_SPI_HOST, &dev_cfg, &s_spi),
                        TAG, "spi add device");
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* SSD1683                                                                   */
/* ------------------------------------------------------------------------- */

/* RAM window, X in bytes (0..49), Y in lines (0..299). */
static esp_err_t ssd1683_set_window(int xb0, int xb1, int y0, int y1)
{
    ESP_RETURN_ON_ERROR(epd_cmd(0x44), TAG, "0x44");
    ESP_RETURN_ON_ERROR(epd_data1((uint8_t)xb0), TAG, "x start");
    ESP_RETURN_ON_ERROR(epd_data1((uint8_t)xb1), TAG, "x end");

    ESP_RETURN_ON_ERROR(epd_cmd(0x45), TAG, "0x45");
    ESP_RETURN_ON_ERROR(epd_data1((uint8_t)(y0 & 0xFF)), TAG, "y start lo");
    ESP_RETURN_ON_ERROR(epd_data1((uint8_t)((y0 >> 8) & 0x01)), TAG, "y start hi");
    ESP_RETURN_ON_ERROR(epd_data1((uint8_t)(y1 & 0xFF)), TAG, "y end lo");
    ESP_RETURN_ON_ERROR(epd_data1((uint8_t)((y1 >> 8) & 0x01)), TAG, "y end hi");
    return ESP_OK;
}

/* Hardware reset + soft reset + the register set a full display sequence
 * needs.
 *
 * Only a full refresh starts from a reset. A partial refresh must keep the
 * controller running, because a reset throws away the 0x26 previous-image
 * plane and the temperature/LUT state; the vendor partial flow (0x22 0xFF)
 * assumes they are still valid. */
static esp_err_t ssd1683_wake(void)
{
    epd_hw_reset();

    /* Soft reset. BUSY is not trustworthy on this build (the wire can float
     * high), so the settling time is fixed instead of polled. */
    ESP_RETURN_ON_ERROR(epd_cmd(0x12), TAG, "soft reset");
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Display update control 1: load normal (not inverted) RAM content */
    ESP_RETURN_ON_ERROR(epd_cmd(0x21), TAG, "0x21");
    ESP_RETURN_ON_ERROR(epd_data1(0x40), TAG, "0x21 data");
    ESP_RETURN_ON_ERROR(epd_data1(0x00), TAG, "0x21 data");

    /* Border waveform */
    ESP_RETURN_ON_ERROR(epd_cmd(0x3C), TAG, "0x3C");
    ESP_RETURN_ON_ERROR(epd_data1(0x05), TAG, "border");

    /* Data entry mode: X+ / Y+, address counter increments per byte */
    ESP_RETURN_ON_ERROR(epd_cmd(0x11), TAG, "0x11");
    ESP_RETURN_ON_ERROR(epd_data1(0x03), TAG, "data entry mode");
    return ESP_OK;
}

static esp_err_t ssd1683_init(void)
{
    ESP_RETURN_ON_ERROR(ssd1683_wake(), TAG, "wake");
    ESP_LOGI(TAG, "BUSY after reset: %d", gpio_get_level(EPD_PIN_BUSY));

    /* MUX gate lines: 300 (0x012B) and the built-in temperature sensor. Both
     * are part of the vendor/GxEPD2 init for this SSD1683 panel; without the
     * temperature sensor the waveform can come out weak. */
    ESP_RETURN_ON_ERROR(epd_cmd(0x01), TAG, "0x01");
    ESP_RETURN_ON_ERROR(epd_data1(0x2B), TAG, "mux lo");
    ESP_RETURN_ON_ERROR(epd_data1(0x01), TAG, "mux hi");
    ESP_RETURN_ON_ERROR(epd_data1(0x00), TAG, "mux gate");

    ESP_RETURN_ON_ERROR(epd_cmd(0x18), TAG, "0x18");
    ESP_RETURN_ON_ERROR(epd_data1(0x80), TAG, "temperature sensor");

    /* RAM window: X in bytes (0..49), Y in lines (0..299) */
    ESP_RETURN_ON_ERROR(ssd1683_set_window(0, EPD_ROW_BYTES - 1, 0, EPD_HEIGHT - 1),
                        TAG, "full window");

    /* RAM address counter back to the origin */
    ESP_RETURN_ON_ERROR(epd_cmd(0x4E), TAG, "0x4E");
    ESP_RETURN_ON_ERROR(epd_data1(0x00), TAG, "x counter");

    ESP_RETURN_ON_ERROR(epd_cmd(0x4F), TAG, "0x4F");
    ESP_RETURN_ON_ERROR(epd_data1(0x00), TAG, "y counter lo");
    ESP_RETURN_ON_ERROR(epd_data1(0x00), TAG, "y counter hi");

    return ESP_OK;
}

static esp_err_t ssd1683_send_image(uint8_t ram_cmd, const uint8_t *image)
{
    /* Whole panel, and the RAM address counter back to the origin. The window
     * is reset here (not right after a refresh) so that no command is sent
     * while the controller may still be updating. */
    ESP_RETURN_ON_ERROR(ssd1683_set_window(0, EPD_ROW_BYTES - 1, 0, EPD_HEIGHT - 1),
                        TAG, "full window");
    ESP_RETURN_ON_ERROR(epd_cmd(0x4E), TAG, "x counter");
    ESP_RETURN_ON_ERROR(epd_data1(0x00), TAG, "x counter");
    ESP_RETURN_ON_ERROR(epd_cmd(0x4F), TAG, "y counter");
    ESP_RETURN_ON_ERROR(epd_data1(0x00), TAG, "y counter lo");
    ESP_RETURN_ON_ERROR(epd_data1(0x00), TAG, "y counter hi");

    ESP_RETURN_ON_ERROR(epd_cmd(ram_cmd), TAG, "write ram");
    return epd_data(image, EPD_BUF_SIZE);
}

/* Full refresh: load the OTP LUT and flash the whole panel. Following the
 * vendor drivers (Waveshare and GxEPD2), the same image is written to both RAM
 * planes before the waveform; the full LUT drives every pixel through its
 * cleaning cycle. Afterwards s_prev holds the frame on the panel, which is the
 * base for the next partial update. */
static esp_err_t ssd1683_update_full(void)
{
    /* The reset is what makes a second and every later activation reliable;
     * see ssd1683_wake(). */
    ESP_RETURN_ON_ERROR(ssd1683_wake(), TAG, "wake");

    ESP_RETURN_ON_ERROR(ssd1683_send_image(0x24, s_fb), TAG, "new image");
    ESP_RETURN_ON_ERROR(ssd1683_send_image(0x26, s_fb), TAG, "previous image");

    /* Restore the full-update settings that a partial update changes. */
    ESP_RETURN_ON_ERROR(epd_cmd(0x3C), TAG, "0x3C");
    ESP_RETURN_ON_ERROR(epd_data1(0x05), TAG, "border");
    ESP_RETURN_ON_ERROR(epd_cmd(0x21), TAG, "0x21");
    ESP_RETURN_ON_ERROR(epd_data1(0x40), TAG, "0x21 data");
    ESP_RETURN_ON_ERROR(epd_data1(0x00), TAG, "0x21 data");

    ESP_RETURN_ON_ERROR(epd_cmd(0x22), TAG, "0x22");
    ESP_RETURN_ON_ERROR(epd_data1(0xF7), TAG, "update sequence");
    ESP_RETURN_ON_ERROR(epd_cmd(0x20), TAG, "activate display");

    /* BUSY floats on this board, so use a fixed, generous delay: cutting a
     * full waveform short leaves the whole panel grey. */
    vTaskDelay(pdMS_TO_TICKS(EPD_REFRESH_MS));

    memcpy(s_prev, s_fb, sizeof(s_prev));
    return ESP_OK;
}

/* Write one rectangle of `image` into one of the two controller RAM planes
 * (0x24 = new image, 0x26 = previous image used by the partial waveform).
 * X is addressed in bytes (0..49), Y in lines (0..299). */
static esp_err_t ssd1683_write_region(uint8_t ram_cmd, const uint8_t *image,
                                      int xb0, int xb1, int y0, int y1)
{
    ESP_RETURN_ON_ERROR(ssd1683_set_window(xb0, xb1, y0, y1), TAG, "region window");

    ESP_RETURN_ON_ERROR(epd_cmd(0x4E), TAG, "0x4E");
    ESP_RETURN_ON_ERROR(epd_data1((uint8_t)xb0), TAG, "x counter");

    ESP_RETURN_ON_ERROR(epd_cmd(0x4F), TAG, "0x4F");
    ESP_RETURN_ON_ERROR(epd_data1((uint8_t)(y0 & 0xFF)), TAG, "y counter lo");
    ESP_RETURN_ON_ERROR(epd_data1((uint8_t)((y0 >> 8) & 0x01)), TAG, "y counter hi");

    ESP_RETURN_ON_ERROR(epd_cmd(ram_cmd), TAG, "write ram");
    size_t len = (size_t)(xb1 - xb0 + 1);
    for (int y = y0; y <= y1; y++) {
        ESP_RETURN_ON_ERROR(epd_data(&image[y * EPD_ROW_BYTES + xb0], len),
                            TAG, "region data");
    }
    return ESP_OK;
}

/* Partial refresh of one rectangle.
 *
 * On this board a partial activation only starts a waveform after a hardware
 * reset: without one, both 0xFC and 0xFF are accepted silently and BUSY never
 * moves. So reset first (same as Waveshare's Arduino example), then run the
 * partial register set.
 *
 * 0x22 = 0xFC (not 0xFF) selects only DISPLAY Mode 1 with the partial LUT.
 * 0xFF also enables DISPLAY Mode 2, which this controller applies to the whole
 * panel: that is the visible full-screen inversion/flash on every minute and
 * the resulting over-driven ghost image. */
static esp_err_t ssd1683_update_region_partial(int xb0, int xb1, int y0, int y1)
{
    epd_hw_reset();
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_RETURN_ON_ERROR(epd_cmd(0x3C), TAG, "0x3C");
    ESP_RETURN_ON_ERROR(epd_data1(0x80), TAG, "partial border");

    ESP_RETURN_ON_ERROR(epd_cmd(0x21), TAG, "0x21");
    ESP_RETURN_ON_ERROR(epd_data1(0x00), TAG, "0x21 data");
    ESP_RETURN_ON_ERROR(epd_data1(0x00), TAG, "0x21 data");

    ESP_RETURN_ON_ERROR(epd_cmd(0x3C), TAG, "0x3C");
    ESP_RETURN_ON_ERROR(epd_data1(0x80), TAG, "partial border");

    /* Keep the "previous image" plane in step with what is actually on the
     * panel. Otherwise the partial LUT compares every update against the last
     * full refresh, and old text stays behind as a ghost (which can merge with
     * the new line into shapes that look like stray letters). */
    ESP_RETURN_ON_ERROR(ssd1683_write_region(0x26, s_prev, xb0, xb1, y0, y1),
                        TAG, "previous image");

    ESP_RETURN_ON_ERROR(ssd1683_write_region(0x24, s_fb, xb0, xb1, y0, y1),
                        TAG, "new image");

    ESP_RETURN_ON_ERROR(epd_cmd(0x22), TAG, "0x22");
    ESP_RETURN_ON_ERROR(epd_data1(0xFC), TAG, "partial sequence");
    ESP_RETURN_ON_ERROR(epd_cmd(0x20), TAG, "activate display");

    /* Diagnostic: log every BUSY edge during the refresh so the real waveform
     * duration becomes visible (the fixed delay below is a guess). */
    int busy = gpio_get_level(EPD_PIN_BUSY);
    for (int ms = 10; ms <= EPD_PARTIAL_MS; ms += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
        int level = gpio_get_level(EPD_PIN_BUSY);
        if (level != busy) {
            ESP_LOGI(TAG, "BUSY %d -> %d at %d ms", busy, level, ms);
            busy = level;
        }
    }
    return ESP_OK;
}

static esp_err_t ssd1683_sleep(void)
{
    ESP_RETURN_ON_ERROR(epd_cmd(0x10), TAG, "deep sleep");
    return epd_data1(0x01);
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

esp_err_t epd_init(void)
{
    static bool bus_ready = false;

    if (!bus_ready) {
        esp_err_t err = epd_bus_init();
        if (err != ESP_OK) {
            return err;
        }
        bus_ready = true;
    } else {
        /* Bus is already configured, just reset the controller. */
    }

    ESP_RETURN_ON_ERROR(ssd1683_init(), TAG, "panel init");

    memset(s_fb, 0xFF, sizeof(s_fb));
    memset(s_prev, 0xFF, sizeof(s_prev));
    ESP_LOGI(TAG, "panel ready: SSD1683, %dx%d", EPD_WIDTH, EPD_HEIGHT);
    return ESP_OK;
}

void epd_clear(void)
{
    memset(s_fb, 0xFF, sizeof(s_fb)); /* all white */
}

void epd_clear_rect(int x, int y, int w, int h)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > EPD_WIDTH ? EPD_WIDTH : x + w;
    int y1 = y + h > EPD_HEIGHT ? EPD_HEIGHT : y + h;

    for (int row = y0; row < y1; row++) {
        for (int col = x0; col < x1; col++) {
            s_fb[row * EPD_ROW_BYTES + (col >> 3)] |= (uint8_t)(0x80 >> (col & 7));
        }
    }
}

void epd_draw_pixel(int x, int y)
{
    if (x < 0 || y < 0 || x >= EPD_WIDTH || y >= EPD_HEIGHT) {
        return;
    }
    s_fb[y * EPD_ROW_BYTES + (x >> 3)] &= (uint8_t)~(0x80 >> (x & 7));
}

void epd_fill_rect(int x, int y, int w, int h)
{
    for (int row = y; row < y + h; row++) {
        for (int col = x; col < x + w; col++) {
            epd_draw_pixel(col, row);
        }
    }
}

esp_err_t epd_update(void)
{
    esp_err_t err = ssd1683_update_full();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "full refresh done");
    }
    return err;
}

esp_err_t epd_update_region(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) {
        return ESP_OK;
    }

    /* Clamp to the panel and snap X outward to whole bytes, because the
     * controller addresses RAM in bytes. */
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > EPD_WIDTH ? EPD_WIDTH : x + w;
    int y1 = y + h > EPD_HEIGHT ? EPD_HEIGHT : y + h;
    if (x1 <= x0 || y1 <= y0) {
        return ESP_OK;
    }

    int xb0 = x0 >> 3;
    int xb1 = (x1 - 1) >> 3;
    int ly0 = y0;
    int ly1 = y1 - 1;

    /* Diagnostic: how many bytes of the refreshed rectangle actually change. */
    int diff = 0;
    for (int row = ly0; row <= ly1; row++) {
        for (int b = xb0; b <= xb1; b++) {
            if (s_fb[row * EPD_ROW_BYTES + b] != s_prev[row * EPD_ROW_BYTES + b]) {
                diff++;
            }
        }
    }

    /* A partial refresh with no changed pixel is not harmless: the controller
     * skips its display phase, and the next real update can be ignored too.
     * Nothing has to be sent when the rectangle is already up to date. */
    if (diff == 0) {
        ESP_LOGI(TAG, "region unchanged (x %d..%d, y %d..%d), skipped",
                 xb0 * 8, xb1 * 8 + 7, ly0, ly1);
        return ESP_OK;
    }

    esp_err_t err = ssd1683_update_region_partial(xb0, xb1, ly0, ly1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "region refresh failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "region refresh done: x %d..%d, y %d..%d, %d bytes changed",
             xb0 * 8, xb1 * 8 + 7, ly0, ly1, diff);

    /* Only this rectangle is now on the panel, so only this rectangle of the
     * snapshot has to advance. (Copying the whole buffer would silently mark
     * other pending rectangles as already displayed.) */
    for (int row = ly0; row <= ly1; row++) {
        memcpy(&s_prev[row * EPD_ROW_BYTES + xb0],
               &s_fb[row * EPD_ROW_BYTES + xb0], (size_t)(xb1 - xb0 + 1));
    }
    return ESP_OK;
}

esp_err_t epd_sleep(void)
{
    return ssd1683_sleep();
}
