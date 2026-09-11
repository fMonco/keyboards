/* ESP32-C6 only. This entire translation unit is linked into RTC memory.
 * No GPIO driver, flash data, heap, or FreeRTOS may be used here. */
#include "esp_sleep.h"
#include "esp_rom_sys.h"
#include "hal/gpio_ll.h"
#include "hal/rtc_io_ll.h"
#include "soc/gpio_sig_map.h"
#include "soc/lp_aon_struct.h"
#include "wake_capture.h"
#include "board_pins.h"
#include "matrix_scan.h"

uint16_t wake_pressed;
/* This file's constants are linked into RTC memory too. */
static const uint8_t wake_cols[] = MATRIX_COL_GPIOS;
static const uint8_t wake_rows[] = {GPIO_NUM_0, GPIO_NUM_1, GPIO_NUM_2};

static void release_all(void)
{
    for (unsigned c = 0; c < 3; c++)
        gpio_ll_output_disable(&GPIO, wake_cols[c]);
}

static void select_column(unsigned c) { gpio_ll_output_enable(&GPIO, wake_cols[c]); }

static uint8_t read_low_rows(void)
{
    uint8_t low = 0;
    for (unsigned r = 0; r < 3; r++)
        if (!gpio_ll_get_level(&GPIO, wake_rows[r])) low |= 1U << r;
    return low;
}

static uint16_t scan(void)
{
    const matrix_io_t io = {release_all, select_column, read_low_rows, esp_rom_delay_us};
    return matrix_scan_cycle(&io).keys;
}

void micropad_wake_stub(void)
{
    /* Запускается раньше загрузчика: запоминает первую кнопку в wake_pressed.
     * app_main() заберёт её даже если пользователь уже отпустил кнопку.
     * Здесь нельзя вызывать обычные драйверы, FreeRTOS и обращаться к flash. */
    esp_default_wake_deep_sleep();
    /* C6 hold bits correspond directly to GPIO numbers. Do not use the
     * GPIO_HOLD_MASK lookup table: it resides in unavailable flash memory. */
    LP_AON.gpio_hold0.gpio_hold0 &= ~((1U << GPIO_NUM_0) | (1U << GPIO_NUM_1) |
                                      (1U << GPIO_NUM_2) | MATRIX_COL_MASK);
    for (unsigned r = 0; r < 3; r++) {
        unsigned row = wake_rows[r];
        // Return the pad from EXT1's RTC mux to the digital scanner.
        rtcio_ll_function_select(row, RTCIO_LL_FUNC_DIGITAL);
        gpio_ll_func_sel(&GPIO, row, PIN_FUNC_GPIO);
        gpio_ll_output_disable(&GPIO, row);
        gpio_ll_input_enable(&GPIO, row);
        gpio_ll_pullup_en(&GPIO, row);
        gpio_ll_pulldown_dis(&GPIO, row);
    }
    for (unsigned column = 0; column < 3; column++) {
        unsigned c = wake_cols[column];
        gpio_ll_func_sel(&GPIO, c, PIN_FUNC_GPIO);
        gpio_ll_set_output_signal_matrix_source(&GPIO, c, SIG_GPIO_OUT_IDX, false);
        gpio_ll_set_output_enable_ctrl(&GPIO, c, false, false);
        gpio_ll_set_level(&GPIO, c, 0);
        gpio_ll_output_disable(&GPIO, c);
        gpio_ll_pullup_en(&GPIO, c);
        gpio_ll_pulldown_dis(&GPIO, c);
    }
    /* Retain only keys stable for 25 ms. This captures a released wake key
     * before the bootloader and Zigbee initialization can hide it. */
    uint16_t stable = scan();
    // 5 пауз по 5000 мкс = 25 мс плюс время сканирования; настройка независима от buttons.h.
    for (unsigned i = 0; i < 5 && stable; i++) {
        esp_rom_delay_us(5000);
        stable &= scan();
    }
    wake_pressed = stable;
}
