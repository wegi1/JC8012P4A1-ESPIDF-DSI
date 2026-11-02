#pragma once

#include <stdint.h> // << TO UTWORZY TYPY uint16_t, uint8_t itd.
#include <stddef.h> // << TO UTWORZY TYP size_t

//============================================================================================================

//============================================================================================================
// bool init_renderer_scratchpad(const uint8_t *font_buffer);
bool init_renderer_scratchpad(const uint8_t *clock_font_buffer, const uint8_t *second_font_buffer);
//============================================================================================================
void safe_clear_screen(uint16_t *buffer, uint16_t color);
//============================================================================================================
void draw_rectangle_90(uint16_t *framebuffer, int native_width, int native_height,
                       int x_pos, int y_pos, int width, int height, uint16_t color);
//============================================================================================================
// Szybka funkcja rysująca linię pionową względem wyświetlanego tekstu
void draw_line_90(uint16_t *framebuffer, int native_width, int native_height,
                  int virtual_x, int virtual_y_start, int virtual_y_end, uint16_t color);
//============================================================================================================
void draw_clock_perfect_monospace_90(uint16_t *framebuffer, int native_width, int native_height,
                                     const uint8_t *font_buffer, const char *text,
                                     int x_pos, int y_pos, float font_size_px, uint16_t text_color);
//============================================================================================================
void draw_text_90(uint16_t *framebuffer, int native_width, int native_height,
                  const char *text, int x_pos, int y_pos,
                  float font_size_px, uint16_t text_color);
//============================================================================================================
//============================================================================================================
//============================================================================================================
