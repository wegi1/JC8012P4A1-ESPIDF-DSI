#pragma once

#include <stdint.h>

void sync_internal_rtc_with_rx8025t(void);
void init_RTC_subsystem(void); // Nowa funkcja inicjalizująca zadania RTC
void print_RTC(void);
