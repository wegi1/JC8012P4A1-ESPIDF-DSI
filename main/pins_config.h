#pragma once

#define LCD_H_RES 800
#define LCD_V_RES 1280

#define LCD_RST 27
#define LCD_LED 23

// 1. Definicja pinów dla dotyku
#define TOUCH_I2C_SDA_PIN GPIO_NUM_7
#define TOUCH_I2C_SCL_PIN GPIO_NUM_8
#define TOUCH_RST_PIN GPIO_NUM_22
#define TOUCH_INT_PIN GPIO_NUM_21
#define TOUCH_I2C_PORT I2C_NUM_0

#define TP_I2C_SDA GPIO_NUM_7
#define TP_I2C_SCL GPIO_NUM_8
#define TP_RST GPIO_NUM_22
#define TP_INT GPIO_NUM_21
