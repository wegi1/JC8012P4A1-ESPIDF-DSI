#pragma once

// JEŻELI PLIK JEST DOŁĄCZANY DO PLIKU C++ (NP. MAIN.CPP),
// PONIŻSZA KLAUZULA MÓWI KOMPILATOROWI C++, ŻE FUNKCJE SĄ W STYLU C.
#ifdef __cplusplus
extern "C"
{
#endif

#include "driver/i2c.h"
#include "pins_config.h"

// ADRESACJA I PORT MAGISTRALI I2C
#define RX8025T_ADDR 0x32
#define RX8025T_I2C_PORT I2C_NUM_0 // WSPÓLNY PORT Z DOTYKIEM GSL3680

// ADRESY REJESTRÓW RTC
#define RX8025T_REG_SEC 0x00
#define RX8025T_REG_MIN 0x01
#define RX8025T_REG_HOUR 0x02
#define RX8025T_REG_WEEK 0x03
#define RX8025T_REG_DAY 0x04
#define RX8025T_REG_MONTH 0x05
#define RX8025T_REG_YEAR 0x06
#define RX8025T_REG_CTRL1 0x0E
#define RX8025T_REG_CTRL2 0x0F

    // STRUKTURA PRZECHOWUJĄCA ODCZYTANY CZAS (KOMPATYBILNA Z SYSTEMAMI RTC)
    typedef struct
    {
        uint8_t second;
        uint8_t minute;
        uint8_t hour;
        uint8_t day;
        uint8_t month;
        uint16_t year;
        uint8_t weekday;
    } rx8025t_time_t;

    // PROTOTYPY FUNKCJI DOSTĘPNE DLA TWOJEGO PROJEKTU
    esp_err_t rx8025t_init(void);
    esp_err_t rx8025t_get_time(rx8025t_time_t *time);
    esp_err_t rx8025t_set_time(const rx8025t_time_t *time);

// ZAMKNIĘCIE BLOKU OCHRONNEGO DLA C++
#ifdef __cplusplus
}
#endif
