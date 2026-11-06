#pragma once

#include <stdint.h>

void sync_internal_rtc_with_rx8025t(void);
void init_RTC_subsystem(void); // Nowa funkcja inicjalizująca zadania RTC
void print_RTC(void);
void set_minutes_menu(void);
void draw_hollow_rectangle_90(uint16_t *buffer, uint16_t buf_w, uint16_t buf_h,
                              uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                              uint16_t thickness, uint16_t color);

// NOWOŚĆ: Przerobiona, w pełni wizualna funkcja edycji minut wywoływana z vTaskTouch
void set_minutes_menu(void);
void wait_for_release(void);
void draw_test_cross(void);
void show_debug_touch(uint16_t touchX, uint16_t touchY);
void vTaskMenuEngine(void *pvParameters);
void draw_common_menu_base(void);
void set_hours_menu(void);
void set_day_menu(void);
void set_month_menu(void);
void set_year_menu(void);
void reset_seconds_to_zero(void);
void reconfig_pin_bl_to_pwm(void);
void set_backlight_brightness(uint8_t percentage);
void set_light_menu(void);
int calc_dow(int y, int m, int d);
const char *getDayofweek(uint8_t number);
void init_NVS_system(void);
void save_brightness_to_NVS(uint8_t brightness_val);
uint8_t load_brightness_from_NVS(void);
uint8_t get_max_days_in_month(int year, uint8_t month);
void vTaskFourHourIncrementer(void *pvParameters);
void init_spiffs_storage(void);
// void wyswietl_jpg_ze_spiffs(const char *sciezka_pliku);
void vTaskAutoPhotoTimer(void *pvParameters);
void wyswietl_bmp_ze_spiffs(const char *sciezka_pliku);
