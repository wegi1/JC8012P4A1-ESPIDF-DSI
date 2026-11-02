#include "rtc.hpp"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include <sys/time.h>
#include "rx8025t.h"
#include "renderer.hpp"

// Definicje globalnych zmiennych stanu (Twoja logika z STM32)
int32_t century = 21;
int32_t SECONDS_TO_CALIBRATE = 0;
uint32_t HOURS_CALIBRATE_PERIOD = 1;
uint8_t UPDATE_FLAG = 0;
uint8_t change_colors = 0;
uint8_t mode_night = 0;

// Struktury czasu systemu ESP-IDF kompatybilne z Twoim kodem
static struct tm time_info;
static time_t now_seconds;
static uint8_t max_day;

// Bufor pomocniczy na teksty (odpowiednik Twojego 'tekst')
static char tekst[64];
