#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Panel geometry: 4.2 inch, 400 x 300, SSD1683 controller (4.2inch e-Paper V2) */
#define EPD_WIDTH       400
#define EPD_HEIGHT      300
#define EPD_ROW_BYTES   (EPD_WIDTH / 8)          /* 50  */
#define EPD_BUF_SIZE    (EPD_ROW_BYTES * EPD_HEIGHT) /* 15000 */

/* Bring up SPI + GPIO and initialise the panel (does not refresh yet).
 * The panel content is undefined until the first epd_update(). */
esp_err_t epd_init(void);

/* Framebuffer helpers. In the framebuffer a set bit means white,
 * a cleared bit means black - same convention as the panel RAM. */
void epd_clear(void);
void epd_clear_rect(int x, int y, int w, int h);
void epd_draw_pixel(int x, int y);
void epd_fill_rect(int x, int y, int w, int h);

/* Push the framebuffer to the panel and run a full refresh (~4 s, the whole
 * panel flashes). Also establishes the base image used by partial updates. */
esp_err_t epd_update(void);

/* Refresh only the given rectangle with the panel's partial waveform (~1-2 s).
 * Only the pixels that differ from the last update are driven, so the panel
 * does not flash. Call epd_update() at least once before the first region
 * update, and again periodically to clear accumulated ghosting.
 *
 * The controller needs both the old and the new image for the rectangle; this
 * is taken from the framebuffer snapshot kept by the driver, so the caller only
 * has to draw the new content with epd_clear_rect()/epd_fill_rect(). */
esp_err_t epd_update_region(int x, int y, int w, int h);

/* Put the controller into deep sleep (content is retained).
 * Only use this when the panel will be powered down afterwards: after deep
 * sleep the controller ignores all commands until it is power cycled. */
esp_err_t epd_sleep(void);
