#pragma once
#include <stdbool.h>
#include <stdint.h>
#define OLED_WIDTH 128
#define OLED_PAGES 8
typedef uint8_t oled_frame_t[OLED_PAGES][OLED_WIDTH];
void oled_frame_clear(oled_frame_t frame);
void oled_frame_text(oled_frame_t frame, unsigned page, const char *text);
/* A contiguous changed column range. Caller commits only after successful I2C. */
bool oled_frame_diff(const uint8_t wanted[OLED_WIDTH], const uint8_t sent[OLED_WIDTH],
                     unsigned *first, unsigned *length);
