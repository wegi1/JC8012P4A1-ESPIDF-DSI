#include "rx8025t.h"
#include "esp_log.h"

static const char *TAG = "RX8025T";

static uint8_t bcd2dec(uint8_t val)
{
    return ((val >> 4) * 10) + (val & 0x0F);
}

static uint8_t dec2bcd(uint8_t val)
{
    return ((val / 10) << 4) | (val % 10);
}

esp_err_t rx8025t_init(void)
{
    // Ustawienie formatu 24h w CTRL1 (bit 5)
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (RX8025T_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, RX8025T_REG_CTRL1, true);
    i2c_master_write_byte(cmd, 0x20, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(RX8025T_I2C_PORT, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Blad komunikacji z RTC w init.");
        return err;
    }

    // Wyczyszczenie flag w CTRL2
    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (RX8025T_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, RX8025T_REG_CTRL2, true);
    i2c_master_write_byte(cmd, 0x00, true);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(RX8025T_I2C_PORT, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);

    ESP_LOGI(TAG, "Konfiguracja rejestrow RX8025T zakonczona.");
    return ESP_OK;
}

esp_err_t rx8025t_get_time(rx8025t_time_t *time)
{
    uint8_t data[7] = {0};

    // Odczyt sekwencyjny od rejestru 0x00 (7 bajtów)
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (RX8025T_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, RX8025T_REG_SEC, true);

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (RX8025T_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, data, 6, I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, &data[6], I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    esp_err_t err = i2c_master_cmd_begin(RX8025T_I2C_PORT, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);

    if (err != ESP_OK)
    {
        return err;
    }

    time->second = bcd2dec(data[0] & 0x7F);
    time->minute = bcd2dec(data[1] & 0x7F);
    time->hour = bcd2dec(data[2] & 0x3F);
    time->weekday = data[3] & 0x07;
    time->day = bcd2dec(data[4] & 0x3F);
    time->month = bcd2dec(data[5] & 0x1F);
    time->year = bcd2dec(data[6]) + 2000;

    return ESP_OK;
}

esp_err_t rx8025t_set_time(const rx8025t_time_t *time)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (RX8025T_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, RX8025T_REG_SEC, true);
    i2c_master_write_byte(cmd, dec2bcd(time->second), true);
    i2c_master_write_byte(cmd, dec2bcd(time->minute), true);
    i2c_master_write_byte(cmd, dec2bcd(time->hour), true);
    i2c_master_write_byte(cmd, time->weekday & 0x07, true);
    i2c_master_write_byte(cmd, dec2bcd(time->day), true);
    i2c_master_write_byte(cmd, dec2bcd(time->month), true);
    i2c_master_write_byte(cmd, dec2bcd(time->year % 100), true);
    i2c_master_stop(cmd);

    esp_err_t err = i2c_master_cmd_begin(RX8025T_I2C_PORT, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
    return err;
}
