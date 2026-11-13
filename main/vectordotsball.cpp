/*
 * main_app.c
 * 1024 v dots with a HUUUGE... ZOOM
 *  Created on: 3 mar 2022
 *      Author: BOLO
 */
//====================
#include <inttypes.h>
#include "vectordotsball.hpp"
#include "tables.hpp"
#include "freertos/FreeRTOS.h"

#include "freertos/semphr.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "jd9365_lcd.h"
#include "esp_timer.h"
//==========================================================
// Zasoby zdefiniowane w main.cpp
extern jd9365_lcd lcd;
extern uint16_t *fbs[1];
extern SemaphoreHandle_t vsync_sem;

// Zasoby zdefiniowane w RTC.cpp
extern volatile bool global_is_touched;
extern void wait_for_release(void);
extern void refresh_menu_self(void);
extern void set_max_briggtness(void);
extern void restore_brightness(void);
extern SemaphoreHandle_t fb_mutex;

//====================
extern volatile uint8_t BRIGHTNESS;
//---

static uint32_t FRAME_ADDRESS = 0;
static uint32_t FRAME_2_ADDRESS = 0;
//======================================

//======================================
#define vector_dots 512
//**************************************
//***************************************
//****************************************

//======================================
#if vector_dots == 512
#define many_circles 8
#else
#define many_circles 16
#endif

#define many_plots many_circles * 2 * 32
//======================================

#define screen_X 800
#define screen_Y 1280
#define zooming ((uint16_t)0x3000)
#define frames_count 500

#define x_min (4096 - screen_X / 2)
#define x_max (4096 + screen_X / 2)

#define y_min (4096 - screen_Y / 2)
#define y_max (4096 + screen_Y / 2)

#define x_offset (4096 - screen_X / 2)
#define y_offset (4096 - screen_Y / 2)

//======================================
static void zoom_manage(void);
static void insert_dots(void);
static void calculate_matrix(void);
static void calculate_8_dots(void);
static void calc_all_dots_01(void);
static void calc_all_dots_02(void);
//======================================

//---
typedef uint8_t multable_row_t[256];

static uint8_t *x_data_valid = nullptr;
static uint8_t *y_data_valid = nullptr;
static multable_row_t *multable = nullptr;

//===
// uint8_t x_data_valid[8192]; // maximum size calculated dot from - 2048 to 2047
// uint8_t y_data_valid[8192]; // after translate to 2D max values are from 0 to 4095
// uint8_t multable[128][256]; // a prescale table
int8_t plot_1X[many_plots]; // first buffer many_plots plot X values
int8_t plot_1Y[many_plots]; // first buffer many_plots plot Y values
//---
static int8_t mat_A, mat_B, mat_C, mat_D, mat_E, mat_F;

static int8_t tb_fact1[8]; // circle prescale factors 4 circles top and 4 mirroring to bottom
static int8_t tb_fact2[8]; //

static int8_t v_5tab[many_circles]; //
static int8_t v_6tab[many_circles]; //
// static int8_t v_7tab[many_circles*2]; //

static int8_t wynik_X[16]; // X poss first 8 plots
static int8_t wynik_Y[16]; // Y poss first 8 plots

static uint32_t plots01[many_plots];
static uint32_t plots02[many_plots];

static uint32_t pl01_calc = 1;
static uint32_t pl02_calc = 1;

static uint16_t zoom_scale = zooming;
static uint8_t zoom_side = 0x01;

static uint8_t zoom = 128; // 128;
static uint32_t frames = 0;

//=========================================================================================================================================

//=============================================
static void zoom_manage(void)
{
    frames++;

    // huge zoom manage
    //====================================================================
    if (frames > frames_count)
    {
        if ((zoom_scale < 0xC000) && (zoom_side != 0))
        {
            zoom_scale += 0x0180;
            if (zoom_scale >= 0xC000)
            {
                zoom_side = 0;
            }
        }
        else
        {
            zoom_scale -= 0x0180;
            if (zoom_scale < 0x0280)
            {
                zoom_side = 1;
            }
        }
        if (frames > 2000)
        {
            zoom_scale = zooming;
            zoom_side = 1;
            frames = 0;
        }
    }
    //====================================================================
}
void start_vectordots_ball(void)
{
    xSemaphoreTake(fb_mutex, portMAX_DELAY);
    FRAME_ADDRESS = (uint32_t)fbs[0];
    void *buf2_ptr = heap_caps_aligned_alloc(64, 800 * 1280 * 2, MALLOC_CAP_SPIRAM);
    FRAME_2_ADDRESS = (uint32_t)buf2_ptr;

    if (multable == nullptr)
    {
        x_data_valid = (uint8_t *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
        y_data_valid = (uint8_t *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
        multable = (multable_row_t *)heap_caps_malloc(128 * 256, MALLOC_CAP_SPIRAM);
    }
    // ... reszta funkcji insert_dots() bez zmian ...

    printf("[PSRAM_CHECK] Adres multable: 0x%08" PRIXPTR " (PSRAM zaczyna sie od 0x48000000)\n", (uintptr_t)multable);
    printf("[PSRAM_CHECK] FRAME_ADDRESS: 0x%08" PRIX32 ", FRAME_2_ADDRESS: 0x%08" PRIX32 "\n", FRAME_ADDRESS, FRAME_2_ADDRESS);

    uint32_t i;

    uint32_t *pFBaddr = (uint32_t *)FRAME_ADDRESS;
    uint32_t *pFBaddr2 = (uint32_t *)FRAME_2_ADDRESS;

    set_max_briggtness();
    global_is_touched = false;

    // clear both framebuffers
    for (i = 0; i < (800 * 1280 / 2); i++)
    {
        pFBaddr[i] = 0;
        pFBaddr2[i] = 0;
    }
    plots02[0] = FRAME_2_ADDRESS;
    plots01[0] = FRAME_ADDRESS;
    insert_dots();
    // SYNCHRONIZACJA Z DMA
    xSemaphoreTake(vsync_sem, 0);
    xSemaphoreTake(vsync_sem, portMAX_DELAY);
    // uint32_t fps_counter = 0;
    // int64_t t_start = esp_timer_get_time();
    while (1)
    {
        //---
        zoom_manage();
        //---

        // --- CLEAR OLD PLOTS IN THE 2ND BUFFER WHICH IS NO INVISIBLE ---
        // --- AND CACHE FLUSH
        for (i = 0; i < pl02_calc; i++)
        {
            *(uint16_t *)(plots02[i]) = 0x0000;
            esp_cache_msync((void *)(plots02[i] & ~63U), 64, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        }

        calculate_matrix();
        calculate_8_dots();
        calc_all_dots_01(); // store plots values in first buffer

        for (i = 0; i < pl02_calc; i++)
        {
            *(uint16_t *)(plots02[i]) = 0xFFFF;
            esp_cache_msync((void *)(plots02[i] & ~63U), 64, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        }

        if (global_is_touched == true)
        {
            for (i = 0; i < (800 * 1280 / 2); i++)
            {
                pFBaddr[i] = 0; // fill SDRAM layer buffors as black color
            }

            vTaskDelay(pdMS_TO_TICKS(20));
            free(buf2_ptr);

            wait_for_release();
            restore_brightness();
            xSemaphoreGive(fb_mutex);
            refresh_menu_self();

            return;
        }
        // esp_cache_msync((void *)FRAME_2_ADDRESS, 800 * 1280 * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        xSemaphoreTake(vsync_sem, portMAX_DELAY);
        lcd.lcd_draw_bitmap(0, 0, 800, 1280, (uint8_t *)FRAME_2_ADDRESS);

        //---
        zoom_manage();
        //---

        // --- CLEAR OLD PLOTS IN THE 1st BUFFER WHICH IS NO INVISIBLE ---
        // --- AND CACHE FLUSH
        for (i = 0; i < pl01_calc; i++)
        {
            *(uint16_t *)(plots01[i]) = 0x0000;
            esp_cache_msync((void *)(plots01[i] & ~63U), 64, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        }

        calculate_matrix();
        calculate_8_dots();
        // store plots values in first buffer
        calc_all_dots_02();
        for (i = 0; i < pl01_calc; i++)
        {
            *(uint16_t *)(plots01[i]) = 0xFFFF;
            esp_cache_msync((void *)(plots01[i] & ~63U), 64, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        }
        // esp_cache_msync((void *)FRAME_ADDRESS, 800 * 1280 * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        xSemaphoreTake(vsync_sem, portMAX_DELAY);
        lcd.lcd_draw_bitmap(0, 0, 800, 1280, (uint8_t *)FRAME_ADDRESS);
        // fps_counter += 2; // bo w jednym obiegu while rysujesz 2 klatki
        // if (fps_counter >= 120)
        // {
        //     int64_t elapsed_us = esp_timer_get_time() - t_start;
        //     float real_fps = (120.0f * 1000000.0f) / elapsed_us;
        //     // printf("[DEMO_BENCH] 120 ramek w %lld ms -> Realny FPS: %.2f\n", elapsed_us / 1000, real_fps);
        //     fps_counter = 0;
        //     t_start = esp_timer_get_time();
        // }
    }
}
//=======================================================================
void insert_dots()
{

    uint8_t i, i2, i3, dana, t9, t10, sx, sy;
    uint8_t t_x;
    uint16_t wynik, x;
    //*************************************************
    // make prescale/multiply table
    dana = 0;
    for (i = 0; i < 128; i++)
    {
        wynik = 0;
        i3 = 0;
        for (i2 = 0; i2 < 128; i2++)
        {
            multable[i][i2] = (uint8_t)(((wynik) >> 8) & 0xFF);
            wynik = wynik + dana;
            i3--;
            multable[i][i3] = (uint8_t)(0 - multable[i][i2]);
        }
        dana = dana + 2;
    }
    //*************************************************
    for (x = 0; x < 8192; x++)
    {
        x_data_valid[x] = 0;
        y_data_valid[x] = 0;
        if ((x > x_min) && (x < x_max))
        {
            x_data_valid[x] = 1;
        }
        if ((x > y_min) && (x < y_max))
        {
            y_data_valid[x] = 2;
        }
    }
    //*************************************************
    //*************************************************
    // calculate and place dots
    t_x = 0x44;
    i2 = 0;
    for (i = 7; i != 0xff; i--)
    {
        t_x = t_x - 0x08;
        sx = t_x & 0x3F;
        sy = 0;
        //;---------------------
        //; F = invsin(t9)-invsin(t10)
        //;---------------------
        t9 = sy - sx;
        t10 = sy + sx;
        tb_fact1[i] = (uint8_t)((sinus[128 + t9] - sinus[128 + t10]) >> 8) & 0xFF;
        tb_fact2[i2] = tb_fact1[i];
        i2++;
    }
    //*************************************************
    // calc data next object #3
#if many_circles == 8
    t_x = 0x44;
#else
    t_x = 0x42;
#endif

    i2 = 0;
    for (i = many_circles - 1; i != 0xff; i--)
    {
#if many_circles == 8
        t_x = t_x - 0x08;
#else
        t_x = t_x - 0x04;
#endif
        sx = t_x & 0x3F;
        sy = 0;
        //;---------------------
        //; F = invsin(t9)-invsin(t10)
        //;---------------------
        t9 = sy - sx;
        t10 = sy + sx;

        v_5tab[i] = (uint8_t)((sinus[128 + t9] - sinus[128 + t10]) >> 8) & 0xFF;
        v_6tab[i2] = v_5tab[i];
        i2++;
    }
}
//===========================================================================
void calculate_matrix()
{
    uint8_t t1, t2, t3, t4, t5, t6, t7, t8, t9, t10;

    static uint16_t delta_x = 0x0060 * 2;
    static uint16_t delta_y = 0x0080 * 2;
    static uint16_t delta_z = 0x0040 * 2;

    static uint16_t angle_x = 0x00;
    static uint16_t angle_y = 0x00;
    static uint16_t angle_z = 0x00;

    static uint8_t sx;
    static uint8_t sy;
    static uint8_t sz;

    /*    update_angles   */
    angle_x = angle_x + delta_x;
    angle_y = angle_y + delta_y;
    angle_z = angle_z + delta_z;

    sx = (uint8_t)((angle_x) >> 8) & 0xFF;
    sy = (uint8_t)((angle_y) >> 8) & 0xFF;
    sz = (uint8_t)((angle_z) >> 8) & 0xFF;

    t1 = sy - sz;
    t2 = sy + sz;
    t3 = sx + sz;
    t4 = sx - sz;
    t5 = sx + sy + sz;
    t6 = sx - sy + sz;
    t7 = sx + sy - sz;
    t8 = sy + sz - sx;
    t9 = sy - sx;
    t10 = sy + sx;

    /*
// cosinus(x)   =  sinus[x+64]
// invsinus(x)  =  sinus[x+128]
;--------------------
; A = cos(t1)+cos(t2)
;--------------------
; B = sin(t1)-sin(t2)
;--------------
; C = invsin(sy)*2
;----------------------------------------------------------
; D = sin(t3)-sin(t4) + (cos(t6)-cos(t5)+cos(t8)-cos(t7))/2
;----------------------------------------------------------
; E = cos(t3)+cos(t4) + (sin(t5)-sin(t6)-sin(t7)-sin(t8))/2
;----------------------------------------------------------
; F = invsin(t9)-invsin(t10)
;---------------------
     */
    mat_A = (uint8_t)((sinus[64 + t1] + sinus[64 + t2]) >> 8) & 0xFF;
    mat_B = (uint8_t)((sinus[t1] - sinus[t2]) >> 8) & 0xFF;
    mat_C = (uint8_t)((sinus[128 + sy] + sinus[128 + sy]) >> 8) & 0xFF;
    mat_D = (uint8_t)((sinus[t3] - sinus[t4] + ((sinus[64 + t6] - sinus[64 + t5] + sinus[64 + t8] - sinus[64 + t7]) / 2)) >> 8) & 0xFF;
    mat_E = (uint8_t)((sinus[64 + t3] + sinus[64 + t4] + ((sinus[t5] - sinus[t6] - sinus[t7] - sinus[t8]) / 2)) >> 8) & 0xFF;
    mat_F = (uint8_t)((sinus[128 + t9] - sinus[128 + t10]) >> 8) & 0xFF;
    if (zoom < 128)
    {
        mat_C = multable[zoom][mat_C];
        mat_F = multable[zoom][mat_F];
    }
}
//====================================================================================================
// --- CALCULATE ALL PLOTS INTO 2nd BUFFER
//========================================================
void calc_all_dots_01(void)
{
    uint16_t row1, row2, i, i2, i3;
    uint8_t add_x, add_y;
    int32_t data1;
    uint32_t helper_y, helper_x;
    int16_t plot_16X;
    int16_t plot_16Y;

    i2 = 0;
    // 4 times calculate and mirroring circles = 4*16*2 = 128 vectordots
    for (i3 = 0; i3 < many_circles; i3++)
    {
        row1 = v_6tab[i3]; // prescale factor for X/Y offset data (add_x;add_y)

        add_x = multable[row1][mat_C]; // X offset  after prescale circle = possitoin X;Y 0;0
        add_y = multable[row1][mat_F]; // Y offset  after prescale circle

        row2 = v_5tab[i3]; // prescale factor for actual circle

        // calculate 32 X;Y values of plots in mirroring 2nd circles
        for (i = 0; i < 16; i++)
        {
            plot_1X[i2] = ((multable[row2][wynik_X[i]] + add_x));
            plot_1X[i2 + 1] = (plot_1X[i2] ^ 255);
            plot_1Y[i2] = ((multable[row2][wynik_Y[i]] + add_y));
            plot_1Y[i2 + 1] = (plot_1Y[i2] ^ 255);

            plot_1X[i2 + 2] = ((multable[row2][wynik_X[i]] - add_x));
            plot_1X[i2 + 3] = (plot_1X[i2 + 2] ^ 255);
            plot_1Y[i2 + 2] = ((multable[row2][wynik_Y[i]] - add_y));
            plot_1Y[i2 + 3] = (plot_1Y[i2 + 2] ^ 255);
            i2 = i2 + 4;
        }
    }
    pl02_calc = 0;
    for (i = 0; i < many_plots; i++)
    {
        plot_16X = plot_1X[i] * 16;
        plot_16Y = plot_1Y[i] * 16;
        data1 = plot_16X * zoom_scale;
        plot_16X = data1 >> 16;
        data1 = plot_16Y * zoom_scale;
        plot_16Y = data1 >> 16;
        plot_16X += 4096;
        plot_16Y += 4096;

        // validate pixel range
        if ((x_data_valid[plot_16X] | y_data_valid[plot_16Y]) == 0x03)
        {
            helper_y = plot_16Y - y_offset;
            helper_x = plot_16X - x_offset;
            //        plots02[pl01_calc] = (FRAME_ADDRESS + (2*((helper_y * screen_X)+ helper_x ))) ;
            plots02[pl02_calc] = ((FRAME_2_ADDRESS) + (((helper_y * screen_X) + helper_x) << 1));
            pl02_calc++;
        }
    }
}
//====================================================================================================
// --- CALCULATE ALL PLOTS INTO 1ST BUFFER = 0xD0000000
//========================================================
void calc_all_dots_02(void)
{
    uint16_t row1, row2, i, i2, i3;
    uint8_t add_x, add_y;
    int32_t data1;
    uint32_t helper_y, helper_x;
    int16_t plot_16X;
    int16_t plot_16Y;

    i2 = 0;
    // 4 times calculate and mirroring circles = 4*16*2 = 128 vectordots
    for (i3 = 0; i3 < many_circles; i3++)
    {
        row1 = v_6tab[i3]; // prescale factor for X/Y offset data (add_x;add_y)

        add_x = multable[row1][mat_C]; // X offset  after prescale circle = possitoin X;Y 0;0
        add_y = multable[row1][mat_F]; // Y offset  after prescale circle

        row2 = v_5tab[i3]; // prescale factor for actual circle

        // calculate 32 X;Y values of plots in mirroring 2nd circles
        for (i = 0; i < 16; i++)
        {
            plot_1X[i2] = ((multable[row2][wynik_X[i]] + add_x));
            plot_1X[i2 + 1] = (plot_1X[i2] ^ 255);
            plot_1Y[i2] = ((multable[row2][wynik_Y[i]] + add_y));
            plot_1Y[i2 + 1] = (plot_1Y[i2] ^ 255);

            plot_1X[i2 + 2] = ((multable[row2][wynik_X[i]] - add_x));
            plot_1X[i2 + 3] = (plot_1X[i2 + 2] ^ 255);
            plot_1Y[i2 + 2] = ((multable[row2][wynik_Y[i]] - add_y));
            plot_1Y[i2 + 3] = (plot_1Y[i2 + 2] ^ 255);
            i2 = i2 + 4;
        }
    }
    pl01_calc = 0;
    for (i = 0; i < many_plots; i++)
    {
        plot_16X = plot_1X[i] * 16;
        plot_16Y = plot_1Y[i] * 16;
        data1 = plot_16X * zoom_scale;
        plot_16X = data1 >> 16;
        data1 = plot_16Y * zoom_scale;
        plot_16Y = data1 >> 16;
        plot_16X += 4096;
        plot_16Y += 4096;

        // validate pixel range
        if ((x_data_valid[plot_16X] | y_data_valid[plot_16Y]) == 0x03)
        {
            helper_y = plot_16Y - y_offset;
            helper_x = plot_16X - x_offset;
            //        plots01[pl01_calc] = (FRAME_ADDRESS + (2*((helper_y * screen_X)+ helper_x ))) ;
            plots01[pl01_calc] = (FRAME_ADDRESS + (((helper_y * screen_X) + helper_x) << 1));
            pl01_calc++;
        }
    }
}
//==================================================================================
void calculate_8_dots()
{
    uint8_t row1, row2;
    uint8_t v1, v2, v4, v5;
    uint8_t i, i2;

    // calculate first 8 dots to following prescale and mirroring
    i2 = 0;
    for (i = 0; i < 8; i++)
    {
        row1 = tb_fact1[i];
        row2 = tb_fact2[i];

        v5 = multable[row1][mat_E];
        v2 = multable[row1][mat_B];
        v4 = multable[row2][mat_D];
        v1 = multable[row2][mat_A];

        wynik_X[i2] = v1 + v2;
        wynik_Y[i2] = v4 + v5;
        wynik_X[i2 + 1] = v1 - v2;
        wynik_Y[i2 + 1] = v4 - v5;
        i2 = i2 + 2;
    }
}
//===============================================================================
