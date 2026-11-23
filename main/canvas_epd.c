#include "canvas.h"

#include "epd4in2.h"

void canvas_fill_rect(int x, int y, int w, int h)
{
    epd_fill_rect(x, y, w, h);
}

void canvas_clear_rect(int x, int y, int w, int h)
{
    epd_clear_rect(x, y, w, h);
}
