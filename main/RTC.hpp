#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

// Deklaracje zmiennych stanu podsystemu RTC
extern int32_t century;
extern int32_t SECONDS_TO_CALIBRATE;
extern uint32_t HOURS_CALIBRATE_PERIOD;
extern uint8_t UPDATE_FLAG;
extern uint8_t change_colors;
extern uint8_t mode_night;

// Podstawowe funkcje odczytu, synchronizacji i prezentacji danych
void read_RTC(void);
void print_RTC(uint16_t *framebuffer);
void sync_internal_rtc_with_rx8025t(void);

// Logika kalendarzowa i matematyczna wyznaczania daty
char *getDayofweek(uint8_t number);
int calc_dow(int y, int m, int d);
void check_RTC_DATE(void);

// Logika zaawansowanej kalibracji i synchronizacji EPOCH
void read_timestamp(void);
void compare_EPOCH(void);
void set_EPOCH(void);
void CHECK_CALIBRATE_TIME_VALUES(void);
