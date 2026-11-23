#pragma once

/* Minimal drawing surface used by the UI code.
 *
 * In the firmware this is backed by the e-paper framebuffer (canvas_epd.c),
 * on the host it is backed by a plain bitmap (tools/preview/canvas_stub.c) so
 * the exact same layout code can be rendered to a PNG for review. */

void canvas_fill_rect(int x, int y, int w, int h);
void canvas_clear_rect(int x, int y, int w, int h);
