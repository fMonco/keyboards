#pragma once
#include <stdint.h>

typedef struct {
    void (*release_all)(void);
    void (*select_column)(unsigned column);
    uint8_t (*read_low_rows)(void);
    void (*delay_us)(uint32_t us);
} matrix_io_t;

typedef struct { uint16_t keys; uint8_t invalid_rows; } matrix_sample_t;

/* The same electrical scan is used before boot and by the application.
 * Read released rows too: LOW without an active column cannot identify a key.
 * Function pointers and callbacks used by the wake stub must reside in RTC. */
static inline matrix_sample_t matrix_scan_cycle(const matrix_io_t *io)
{
    /* Паузы здесь в МИКРОсекундах, не мс: 150 на восстановление HIGH,
     * 100 на установление LOW выбранного столбца. Одинаковы до и после boot. */
    matrix_sample_t result = {0};
    io->release_all();
    io->delay_us(150);
    result.invalid_rows = io->read_low_rows(); // LOW без выбранного столбца — недостоверная строка.
    for (unsigned c = 0; c < 3; c++) {
        io->select_column(c);
        io->delay_us(100);
        uint8_t low = io->read_low_rows();
        for (unsigned r = 0; r < 3; r++)
            if (low & (1U << r)) result.keys |= 1U << (r * 3 + c);
        io->release_all();
        io->delay_us(150);
        result.invalid_rows |= io->read_low_rows();
    }
    for (unsigned r = 0; r < 3; r++)
        if (result.invalid_rows & (1U << r)) result.keys &= ~(7U << (r * 3));
    return result;
}
