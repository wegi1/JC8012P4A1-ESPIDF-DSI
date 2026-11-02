#include "renderer.hpp"
#include <stdio.h>
#include <string.h>
#include "pins_config.h" // Zawiera definicje LCD_H_RES i LCD_V_RES
#include "esp_heap_caps.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

//============================================================================================================
//============================================================================================================
//============================================================================================================
// Dwie globalne, raz zainicjalizowane czcionki – pełna wydajność!
stbtt_fontinfo global_clock_font;
stbtt_fontinfo global_second_font;

//============================================================================================================
//============================================================================================================
//============================================================================================================
// STAŁY BUFOR ROBOCZY 1 MB W PSRAM
#define STB_SCRATCHPAD_SIZE (1024 * 1024)
static uint8_t *stb_scratchpad_ptr = NULL;

bool init_renderer_scratchpad(const uint8_t *clock_font_buffer, const uint8_t *second_font_buffer)
{
    // 1. Alokacja 1 MB bufora z wyrównaniem do 64 bajtów pod L2 Cache
    stb_scratchpad_ptr = (uint8_t *)heap_caps_aligned_alloc(64, STB_SCRATCHPAD_SIZE, MALLOC_CAP_SPIRAM);
    if (stb_scratchpad_ptr == NULL)
    {
        printf("[ERROR] Nie udalo sie zaalokowac 1MB w PSRAM dla STB!\n");
        return false;
    }

    // 2. Parsowanie PIERWSZEJ czcionki (Zegar)
    if (!stbtt_InitFont(&global_clock_font, clock_font_buffer, 0))
    {
        printf("[ERROR] Nie udalo sie zainicjalizowac global_clock_font!\n");
        return false;
    }

    // 3. Parsowanie DRUGIEJ czcionki (Demo / Menu)
    if (!stbtt_InitFont(&global_second_font, second_font_buffer, 0))
    {
        printf("[ERROR] Nie udalo sie zainicjalizowac global_second_font!\n");
        return false;
    }

    printf("[MEMORY] STB zakres: %p -> %p (Rozmiar: %d bajtow)\n",
           (void *)stb_scratchpad_ptr,
           (void *)(stb_scratchpad_ptr + STB_SCRATCHPAD_SIZE - 1),
           STB_SCRATCHPAD_SIZE);

    printf("[SUCCESS] Obie czcionki zostaly pomyslnie sparsowane na starcie.\n");
    return true;
}

//============================================================================================================
//============================================================================================================
//============================================================================================================
void safe_clear_screen(uint16_t *buffer, uint16_t color)
{
    uint16_t raw_rgb_color = color;
    size_t total_pixels = LCD_H_RES * LCD_V_RES;
    for (size_t i = 0; i < total_pixels; i++)
    {
        buffer[i] = raw_rgb_color;
    }
}
//============================================================================================================
//============================================================================================================
//============================================================================================================
//============================================================================================================
//============================================================================================================
//============================================================================================================
// Szybka procedura rysowania wypełnionego prostokąta w układzie poziomym (Obrót o 90 stopni)
// Współrzędne podajesz w wirtualnej siatce poziomej 1280 (szerokość) x 800 (wysokość)
void draw_rectangle_90(uint16_t *framebuffer, int native_width, int native_height,
                       int x_pos, int y_pos, int width, int height, uint16_t color)
{
    // Iterujemy po wirtualnej szerokości i wysokości prostokąta
    for (int v_x = x_pos; v_x < x_pos + width; v_x++)
    {
        for (int v_y = y_pos; v_y < y_pos + height; v_y++)
        {

            // Dokładnie ta sama transformacja obrotu, co dla czcionek i linii!
            int scr_x = native_width - 1 - v_y;
            int scr_y = v_x;

            // Bezpiecznik przed wyjściem poza fizyczną pamięć matrycy (800x1280)
            if (scr_x >= 0 && scr_x < native_width && scr_y >= 0 && scr_y < native_height)
            {
                framebuffer[scr_y * native_width + scr_x] = color;
            }
        }
    }
}

//============================================================================================================
//============================================================================================================
//============================================================================================================
// Szybka funkcja rysująca linię pionową względem wyświetlanego tekstu
void draw_line_90(uint16_t *framebuffer, int native_width, int native_height,
                  int virtual_x, int virtual_y_start, int virtual_y_end, uint16_t color)
{
    for (int v_y = virtual_y_start; v_y <= virtual_y_end; v_y++)
    {
        // Ta sama transformacja obrotu o 90 stopni, co dla czcionki!
        int scr_x = native_width - 1 - v_y;
        int scr_y = virtual_x;

        // Bezpiecznik pamięci
        if (scr_x >= 0 && scr_x < native_width && scr_y >= 0 && scr_y < native_height)
        {
            framebuffer[scr_y * native_width + scr_x] = color;
        }
    }
}
//============================================================================================================
//============================================================================================================
//============================================================================================================
void draw_clock_perfect_monospace_90(uint16_t *framebuffer, int native_width, int native_height,
                                     const uint8_t *font_buffer, const char *text,
                                     int x_pos, int y_pos, float font_size_px, uint16_t text_color)
{

    float scale = stbtt_ScaleForPixelHeight(&global_clock_font, font_size_px);

    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&global_clock_font, &ascent, &descent, &lineGap);
    int baseline = (int)(ascent * scale);

    // KROK 1: Wyznaczamy idealną, stałą szerokość wirtualnego pudełka na bazie cyfry '0'
    int zero_advance, zero_lsb;
    stbtt_GetCodepointHMetrics(&global_clock_font, '0', &zero_advance, &zero_lsb);
    int box_width = (int)(zero_advance * scale); // Nasz stały "buks" na cyfrę

    // KROK 2: Pobieramy fabryczny margines cyfry '0', aby pierwszy box przytulił się idealnie do X = 0
    int zero_x1, zero_y1, zero_x2, zero_y2;
    stbtt_GetCodepointBitmapBox(&global_clock_font, '0', scale, scale, &zero_x1, &zero_y1, &zero_x2, &zero_y2);
    int global_left_offset = (int)(zero_lsb * scale);

    int x = x_pos - global_left_offset; // Korekta startu, żeby box zaczynał się od piksela 0

    for (int i = 0; text[i] != '\0'; ++i)
    {
        int cp = text[i];

        int c_x1, c_y1, c_x2, c_y2;
        stbtt_GetCodepointBitmapBox(&global_clock_font, cp, scale, scale, &c_x1, &c_y1, &c_x2, &c_y2);

        int y = y_pos + baseline + c_y1;
        int gw = c_x2 - c_x1; // Rzeczywista szerokość rysowanego znaku
        int gh = c_y2 - c_y1; // Rzeczywista wysokość rysowanego znaku

        // KROK 3: Logika wyrównania do prawej krawędzi WIRTUALNEGO PUDEŁKA (box_width)
        int right_align_shift = 0;
        if (cp != ':')
        {
            // Obliczamy ile wolnego miejsca zostaje w pudełku i przesuwamy cyfrę w prawo
            int current_advance, current_lsb;
            stbtt_GetCodepointHMetrics(&global_clock_font, cp, &current_advance, &current_lsb);
            int current_width = (int)(current_advance * scale);
            right_align_shift = box_width - current_width;
        }
        //--------------------------
        if (gw > 0 && gh > 0)
        {
            // BEZPIECZNIK: Sprawdzamy czy pojedynczy znak nie przekracza 1 MB
            if ((gw * gh) > (1024 * 1024))
            {
                printf("Blad: Glif przekracza rozmiar bufora roboczego!\n");
                continue;
            }

            // ZASADA ZERO-MALLOC: Zamiast malloc, używamy naszego stałego wskaźnika z PSRAM
            uint8_t *bitmap = stb_scratchpad_ptr;

            stbtt_MakeCodepointBitmap(&global_clock_font, bitmap, gw, gh, gw, scale, scale, cp);

            for (int bx = 0; bx < gw; ++bx)
            {
                for (int by = 0; by < gh; ++by)
                {
                    uint8_t alpha = bitmap[by * gw + bx];

                    if (alpha > 30)
                    {
                        int virtual_x = x + bx + c_x1 + right_align_shift;
                        int virtual_y = y + by;

                        int scr_x = native_width - 1 - virtual_y;
                        int scr_y = virtual_x;

                        if (scr_x >= 0 && scr_x < native_width && scr_y >= 0 && scr_y < native_height)
                        {
                            framebuffer[scr_y * native_width + scr_x] = text_color;
                        }
                    }
                }
            }
            // ZASADA ZERO-MALLOC: Usuwamy stąd funkcję free(bitmap), bo pamięć w PSRAM ma żyć wiecznie!
        }

        // KROK 4: Kursor przesuwa się zawsze o stałą szerokość wirtualnego pudełka!
        if (cp == ':')
        {
            // Dwukropek dostaje mniejszy, sztywny krok, żeby nie robić gigantycznej dziury
            int colon_advance, colon_lsb;
            stbtt_GetCodepointHMetrics(&global_clock_font, ':', &colon_advance, &colon_lsb);
            x += (int)(colon_advance * scale);
        }
        else
        {
            x += box_width; // Cyfry idą idealnie jak w siatce matrycowej!
        }
    }
}
//============================================================================================================
//============================================================================================================
//============================================================================================================
void draw_text_90(uint16_t *framebuffer, int native_width, int native_height,
                  const char *text, int x_pos, int y_pos,
                  float font_size_px, uint16_t text_color)
{

    float scale = stbtt_ScaleForPixelHeight(&global_second_font, font_size_px);

    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&global_second_font, &ascent, &descent, &lineGap);

    // Zapobiegamy ucinaniu góry znaku przy y_pos = 0 dla bardzo dużych rozmiarów
    int baseline = (int)(ascent * scale);
    int x = x_pos;

    for (int i = 0; text[i] != '\0'; ++i)
    {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&global_second_font, text[i], &advance, &lsb);

        int c_x1, c_y1, c_x2, c_y2;
        stbtt_GetCodepointBitmapBox(&global_second_font, text[i], scale, scale, &c_x1, &c_y1, &c_x2, &c_y2);

        // Korekta pozycjonowania Y
        int y = y_pos + baseline + c_y1;
        int byte_width = c_x2 - c_x1;
        int byte_height = c_y2 - c_y1;

        if (byte_width > 0 && byte_height > 0)
        {
            uint8_t *bitmap = stb_scratchpad_ptr;
            stbtt_MakeCodepointBitmap(&global_second_font, bitmap, byte_width, byte_height, byte_width, scale, scale, text[i]);

            for (int bx = 0; bx < byte_width; ++bx)
            {
                for (int by = 0; by < byte_height; ++by)
                {
                    uint8_t alpha = bitmap[by * byte_width + bx];

                    if (alpha > 30)
                    {
                        int virtual_x = x + bx + c_x1;
                        int virtual_y = y + by;

                        int scr_x = native_width - 1 - virtual_y;
                        int scr_y = virtual_x;

                        if (scr_x >= 0 && scr_x < native_width && scr_y >= 0 && scr_y < native_height)
                        {
                            framebuffer[scr_y * native_width + scr_x] = text_color;
                        }
                    }
                }
            }
        }
        x += (int)(advance * scale);
        if (text[i + 1] != '\0')
        {
            x += (int)(stbtt_GetCodepointKernAdvance(&global_second_font, text[i], text[i + 1]) * scale);
        }
    }
}
//============================================================================================================

//============================================================================================================
//============================================================================================================
//============================================================================================================
