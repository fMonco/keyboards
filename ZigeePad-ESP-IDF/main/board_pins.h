#pragma once

/* XIAO ESP32-C6: D7=GPIO17, D8=GPIO19, D9=GPIO20 (not consecutive).
 * Shared by normal scanning and the RTC wake stub. */
#define MATRIX_COL0_GPIO 17 // D7: столбец кнопок 1, 4, 7.
#define MATRIX_COL1_GPIO 19 // D8: столбец кнопок 2, 5, 8.
#define MATRIX_COL2_GPIO 20 // D9: столбец кнопок 3, 6, 9.
#define MATRIX_COL_GPIOS { MATRIX_COL0_GPIO, MATRIX_COL1_GPIO, MATRIX_COL2_GPIO }
#define MATRIX_COL_MASK ((1U << MATRIX_COL0_GPIO) | (1U << MATRIX_COL1_GPIO) | (1U << MATRIX_COL2_GPIO))
