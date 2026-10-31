// #define _GNU_SOURCE // << TO MAKRO MUSI BYĆ JAKO PIERWSZE NA SAMEJ GÓRZE PLIKU!

#pragma GCC push_options
#pragma GCC optimize("O3")

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

#include "pins_config.h"
#include "jd9365_lcd.h"
#include "driver/i2c.h"

// 1. Dołączamy Twoje tablice z czcionkami bezpośrednio z folderu fonts
#include "fonty.h"
#include "boldfonts.h"

// 2. Aktywujemy i dołączamy silnik stb_truetype
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <sys/time.h> // Zawiera settimeofday
#include <time.h>     // Zawiera struktury czasu

//============================================================================================================
//============================================================================================================
//============================================================================================================

// Wymuszenie dla kompilatora C++, aby traktował plik producenta jako kod C++ / C kompatybilny
extern "C"
{
#include "gsl3680_touch.h"
#include "rx8025t.h"
}
//============================================================================================================
//============================================================================================================
//============================================================================================================

#include "driver/i2c.h" // Potrzebne do konfiguracji magistrali

jd9365_lcd lcd(LCD_RST);

// JEDEN STAŁY BUFOR W PSRAM DLA CAŁEGO INTERFEJSU
uint16_t *backbuffer = NULL;
//============================================================================================================
//============================================================================================================
//============================================================================================================
void safe_clear_screen(uint16_t color)
{
    uint16_t raw_rgb_color = color;
    size_t total_pixels = LCD_H_RES * LCD_V_RES;
    for (size_t i = 0; i < total_pixels; i++)
    {
        backbuffer[i] = raw_rgb_color;
    }
}
//============================================================================================================
//============================================================================================================
//============================================================================================================
void sync_internal_rtc_with_rx8025t(void)
{
    rx8025t_time_t rtc_hardware_time;

    // 1. Pobieramy aktualny czas z fizycznej kostki RX8025T
    if (rx8025t_get_time(&rtc_hardware_time) == ESP_OK)
    {
        // 2. Przepisujemy na strukturę systemową 'struct tm'
        struct tm sys_time;
        sys_time.tm_sec = rtc_hardware_time.second;
        sys_time.tm_min = rtc_hardware_time.minute;
        sys_time.tm_hour = rtc_hardware_time.hour;
        sys_time.tm_mday = rtc_hardware_time.day;
        sys_time.tm_mon = rtc_hardware_time.month - 1;    // W systemie C miesiące są od 0 do 11
        sys_time.tm_year = rtc_hardware_time.year - 1900; // System C liczy lata od roku 1900
        sys_time.tm_wday = rtc_hardware_time.weekday;
        sys_time.tm_isdst = -1; // Brak informacji o czasie letnim

        // 3. Konwersja na znacznik czasu UNIX (liczba sekund od 1970 r.)
        time_t t = mktime(&sys_time);

        // 4. Ustawiamy wewnętrzny czas systemowy w ESP32-P4
        struct timeval now = {.tv_sec = t, .tv_usec = 0};
        settimeofday(&now, NULL);

        ESP_LOGI("SYSTEM_RTC", "Synchronizacja udana! Wewnetrzny zegar ESP32-P4 zostal ustawiony.");
    }
    else
    {
        ESP_LOGE("SYSTEM_RTC", "BLAD SYNCHRONIZACJI! Zewnetrzny RX8025T nie odpowiedzial.");
    }
}

//============================================================================================================
//============================================================================================================
//============================================================================================================
void draw_text_90(uint16_t *framebuffer, int native_width, int native_height,
                  const uint8_t *font_buffer, const char *text,
                  int x_pos, int y_pos, float font_size_px, uint16_t text_color)
{
    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, font_buffer, 0))
    {
        printf("Błąd inicjalizacji czcionki!\n");
        return;
    }

    float scale = stbtt_ScaleForPixelHeight(&font, font_size_px);

    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &lineGap);

    // Zapobiegamy ucinaniu góry znaku przy y_pos = 0 dla bardzo dużych rozmiarów
    int baseline = (int)(ascent * scale);
    int x = x_pos;

    for (int i = 0; text[i] != '\0'; ++i)
    {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&font, text[i], &advance, &lsb);

        int c_x1, c_y1, c_x2, c_y2;
        stbtt_GetCodepointBitmapBox(&font, text[i], scale, scale, &c_x1, &c_y1, &c_x2, &c_y2);

        // Korekta pozycjonowania Y
        int y = y_pos + baseline + c_y1;
        int byte_width = c_x2 - c_x1;
        int byte_height = c_y2 - c_y1;

        if (byte_width > 0 && byte_height > 0)
        {
            uint8_t *bitmap = (uint8_t *)malloc(byte_width * byte_height);
            stbtt_MakeCodepointBitmap(&font, bitmap, byte_width, byte_height, byte_width, scale, scale, text[i]);

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
            free(bitmap);
        }
        x += (int)(advance * scale);
        if (text[i + 1] != '\0')
        {
            x += (int)(stbtt_GetCodepointKernAdvance(&font, text[i], text[i + 1]) * scale);
        }
    }
}
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
    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, font_buffer, 0))
    {
        printf("Błąd inicjalizacji czcionki!\n");
        return;
    }

    float scale = stbtt_ScaleForPixelHeight(&font, font_size_px);

    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &lineGap);
    int baseline = (int)(ascent * scale);

    // KROK 1: Wyznaczamy idealną, stałą szerokość wirtualnego pudełka na bazie cyfry '0'
    int zero_advance, zero_lsb;
    stbtt_GetCodepointHMetrics(&font, '0', &zero_advance, &zero_lsb);
    int box_width = (int)(zero_advance * scale); // Nasz stały "buks" na cyfrę

    // KROK 2: Pobieramy fabryczny margines cyfry '0', aby pierwszy box przytulił się idealnie do X = 0
    int zero_x1, zero_y1, zero_x2, zero_y2;
    stbtt_GetCodepointBitmapBox(&font, '0', scale, scale, &zero_x1, &zero_y1, &zero_x2, &zero_y2);
    int global_left_offset = (int)(zero_lsb * scale);

    int x = x_pos - global_left_offset; // Korekta startu, żeby box zaczynał się od piksela 0

    for (int i = 0; text[i] != '\0'; ++i)
    {
        int cp = text[i];

        int c_x1, c_y1, c_x2, c_y2;
        stbtt_GetCodepointBitmapBox(&font, cp, scale, scale, &c_x1, &c_y1, &c_x2, &c_y2);

        int y = y_pos + baseline + c_y1;
        int gw = c_x2 - c_x1; // Rzeczywista szerokość rysowanego znaku
        int gh = c_y2 - c_y1; // Rzeczywista wysokość rysowanego znaku

        // KROK 3: Logika wyrównania do prawej krawędzi WIRTUALNEGO PUDEŁKA (box_width)
        int right_align_shift = 0;
        if (cp != ':')
        {
            // Obliczamy ile wolnego miejsca zostaje w pudełku i przesuwamy cyfrę w prawo
            int current_advance, current_lsb;
            stbtt_GetCodepointHMetrics(&font, cp, &current_advance, &current_lsb);
            int current_width = (int)(current_advance * scale);
            right_align_shift = box_width - current_width;
        }

        if (gw > 0 && gh > 0)
        {
            uint8_t *bitmap = (uint8_t *)malloc(gw * gh);
            stbtt_MakeCodepointBitmap(&font, bitmap, gw, gh, gw, scale, scale, cp);

            for (int bx = 0; bx < gw; ++bx)
            {
                for (int by = 0; by < gh; ++by)
                {
                    uint8_t alpha = bitmap[by * gw + bx];

                    if (alpha > 30)
                    {
                        // Pozycja uwzględnia pozycję boksu oraz wyrównanie znaku do jego prawej strony
                        int virtual_x = x + bx + c_x1 + right_align_shift;
                        int virtual_y = y + by;

                        // Obrót o 90 stopni do poziomu
                        int scr_x = native_width - 1 - virtual_y;
                        int scr_y = virtual_x;

                        if (scr_x >= 0 && scr_x < native_width && scr_y >= 0 && scr_y < native_height)
                        {
                            framebuffer[scr_y * native_width + scr_x] = text_color;
                        }
                    }
                }
            }
            free(bitmap);
        }

        // KROK 4: Kursor przesuwa się zawsze o stałą szerokość wirtualnego pudełka!
        if (cp == ':')
        {
            // Dwukropek dostaje mniejszy, sztywny krok, żeby nie robić gigantycznej dziury
            int colon_advance, colon_lsb;
            stbtt_GetCodepointHMetrics(&font, ':', &colon_advance, &colon_lsb);
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
extern "C" void app_main(void)
{
    printf("[LOG] INICJALIZACJA SYSTEMU BARE-METAL...\n");
    lcd.begin();

    // ZAALOKUJ BUFOR W PSRAM TYLKO RAZ NA STARTE SYSTEMU (OK. 2 MB)
    size_t buffer_size = LCD_H_RES * LCD_V_RES * sizeof(uint16_t);
    backbuffer = (uint16_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);

    if (backbuffer == NULL)
    {
        printf("[CRITICAL ERROR] Brak pamieci PSRAM na Backbuffer!\n");
        return;
    }
    printf("[LOG] STABILNA PĘTLA GRAFICZNA URUCHOMIONA!\n");

    // 2. Deklaracja obiektu (producent sam skonfiguruje I2C w środku)
    static gsl3680_touch touch(TP_I2C_SDA, TP_I2C_SCL, TP_RST, TP_INT);

    // 3. Odpalenie dotyku – teraz przejdzie bez błędu i wybudzi układ!
    touch.begin();
    printf("[LOG] Dotyk gsl3680 pomyślnie wystartował bez konfliktów magistrali!\n");
    //============================================================================================================
    rx8025t_init();
    sync_internal_rtc_with_rx8025t(); // <<< WYWOŁUJEMY RAZ PO RESECIE

    // =========================================================================
    // TEST DETEKCJI I ODCZYTU ZEGARA RTC RX8025T
    // =========================================================================

    // Szybki ping adresu I2C (0x32) w celu weryfikacji obecności układu na magistrali
    i2c_cmd_handle_t ping_cmd = i2c_cmd_link_create();
    i2c_master_start(ping_cmd);
    i2c_master_write_byte(ping_cmd, (0x32 << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(ping_cmd);

    esp_err_t ping_err = i2c_master_cmd_begin(I2C_NUM_0, ping_cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(ping_cmd);

    if (ping_err == ESP_OK)
    {
        ESP_LOGI("RTC_TEST", "SUKCES! Wykryto urzadzenie RX8025T pod adresem I2C 0x32.");

        // Jeśli układ żyje, spróbujmy odczytać z niego czas na starcie
        rx8025t_time_t test_time;
        if (rx8025t_get_time(&test_time) == ESP_OK)
        {
            ESP_LOGI("RTC_TEST", "Pierwszy odczyt czasu udany: %02d:%02d:%02d, Data: %02d-%02d-%04d",
                     test_time.hour, test_time.minute, test_time.second,
                     test_time.day, test_time.month, test_time.year);
        }
        else
        {
            ESP_LOGE("RTC_TEST", "Blad podczas parsowania rejestrow czasu!");
        }
    }
    else
    {
        ESP_LOGE("RTC_TEST", "BLAD! Urzadzenie pod adresem 0x32 nie odpowiada na szynie I2C. Sprawdz zasilanie/polaczenie.");
    }
    // ---------------------------------------------------------------------
    // JEDNORAZOWE USTAWIANIE AKTUALNEGO CZASU
    // ---------------------------------------------------------------------
    // rx8025t_time_t czas_na_start;
    // czas_na_start.hour = 10;   // Aktualna godzina (Format 24h)
    // czas_na_start.minute = 41; // Aktualna minuta
    // czas_na_start.second = 0;  // Sekundy na zero
    // czas_na_start.day = 31;    // 31 dzień miesiąca
    // czas_na_start.month = 10;  // Październik
    // czas_na_start.year = 2026; // Rok 2026
    // czas_na_start.weekday = 6; // 6 -> Sobota (licząc od 0=Niedziela lub 1=Poniedziałek wg Twojej konwencji)

    // if (rx8025t_set_time(&czas_na_start) == ESP_OK)
    // {
    //     ESP_LOGI("RTC_TEST", "Prawidlowy czas (31.10.2026 10:44) zapisany do pamieci RTC!");
    // }
    // else
    // {
    //     ESP_LOGE("RTC_TEST", "Blad zapisu czasu!");
    // }
    // ---------------------------------------------------------------------

    // =========================================================================

    //============================================================================================================
    uint16_t touchX = 0;
    uint16_t touchY = 0;

    // 1. CZYSZCZENIE EKRANU W PSRAM -> CZERWONY

    while (1)
    {
        // 1. CZYSZCZENIE EKRANU W PSRAM -> CZERWONY
        printf("[LOG] RENDER -> CZERWONY\n");
        safe_clear_screen(0xF800);

        // JEDEN POTĘŻNY, STABILNY TRANSFER SPRZĘTOWY PRZEZ DMA (800x1280)
        lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)backbuffer);

        //============================================================================================================
        //============================================================================================================
        //============================================================================================================
        uint64_t start_time = 0;
        uint64_t end_time = 0;

        // Zmienne do obsługi wewnętrznego zegara systemowego ESP32-P4
        time_t now_seconds;
        struct tm time_info;

        char str_godzina[4];
        char str_minuta[4];
        char str_sekunda[4];

        while (1)
        {
            // ODCZYT CZASU Z WEWNĘTRZNEGO RTC PROCESORA ESP32-P4 (Trwa ułamki mikrosekundy!)
            start_time = esp_timer_get_time(); // Pomiar wydajności renderu
            time(&now_seconds);
            localtime_r(&now_seconds, &time_info);

            // Formatujemy bufory z wewnętrznej struktury systemowej 'time_info'
            sprintf(str_godzina, "%02d", time_info.tm_hour);
            sprintf(str_minuta, "%02d", time_info.tm_min);
            sprintf(str_sekunda, "%02d", time_info.tm_sec);

            safe_clear_screen(0x0000); // Czyszczenie na czarno

            // 1. RYSOWANIE GODZINY (Rozmiar 370, wirtualny start X = 0, Y = 0)
            draw_clock_perfect_monospace_90(backbuffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, str_godzina, 0, 0, 370.0f, 0xFFFF);

            // 2. RYSOWANIE MINUTY (Rozmiar 370, wirtualny dół X = 680, Y = 0)
            draw_clock_perfect_monospace_90(backbuffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, str_minuta, 680, 0, 370.0f, 0xFFFF);

            // 3. PIONOWA ZIELONA LINIA BAZOWA NA WIRTUALNYM Y = 600
            // draw_rectangle_90(backbuffer, LCD_H_RES, LCD_V_RES, 0, 600, 799, 2, 0x07E0);

            // 4. RYSOWANIE SEKUNDNIKA (Wielkość 100, wirtualny Y = 610, wysokość X = 140)
            draw_text_90(backbuffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, str_sekunda, 140, 610, 100.0f, 0xFFFF);

            // Migające kwadraciki dwukropka na X = 605
            draw_rectangle_90(backbuffer, LCD_H_RES, LCD_V_RES, 605, 138, 50, 50, 0xFFFF);
            draw_rectangle_90(backbuffer, LCD_H_RES, LCD_V_RES, 605, 324, 50, 50, 0xFFFF);

            // Przesłanie bufora na ekran MIPI-DSI
            lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)backbuffer);

            end_time = esp_timer_get_time();

            // Zobaczysz teraz w profilerze, ile czasu zaoszczędziliśmy na braku komunikacji po I2C!
            printf("[PROFILER] Render calosci z wewnetrznego ZEGARA P4 zajal: %llu us\n", (end_time - start_time));

            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        //============================================================================================================
        //============================================================================================================
        //============================================================================================================

        // 3. Twoja pętla główna programu
        while (1)
        {
            // Sprawdzamy czy ekran jest dotykany za pomocą metody producenta
            // przekazując wskaźniki do naszych zmiennych X i Y
            bool touched = touch.getTouch(&touchX, &touchY);

            if (touched)
            {
                // Jeśli wykryto dotyk, wypisujemy współrzędne na konsolę (zakładka MONITOR)
                printf("DOTYK! X = %d, Y = %d\n", touchX, touchY);
            }

            // Krótkie opóźnienie 20ms, aby nie przeciążać procesora i magistrali
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        // 2. CZYSZCZENIE EKRANU W PSRAM -> NIEBIESKI
        printf("[LOG] RENDER -> NIEBIESKI\n");
        safe_clear_screen(0x001F);

        lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)backbuffer);
        vTaskDelay(pdMS_TO_TICKS(1000));

        printf("[LOG] RENDER -> ZIELONY\n");
        safe_clear_screen(0x07E0);

        lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)backbuffer);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}