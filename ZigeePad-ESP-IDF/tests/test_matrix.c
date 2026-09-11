#include <assert.h>
#include <stdio.h>
#include "../main/matrix_scan.h"

static int selected;
static unsigned settled;
static uint16_t physical;
static uint8_t stuck;

static void release(void) { selected = -1; settled = 0; }
static void select_col(unsigned c) { assert(selected == -1); selected = (int)c; settled = 0; }
static void delay(uint32_t us) { settled += us; }
static uint8_t read_rows(void)
{
    assert(settled >= (selected < 0 ? 150U : 100U));
    uint8_t low = stuck;
    if (selected >= 0)
        for (unsigned r = 0; r < 3; r++)
            if (physical & (1U << (r * 3 + selected))) low |= 1U << r;
    return low;
}

int main(void)
{
    const matrix_io_t io = {release, select_col, read_rows, delay};
    for (physical = 0; physical < 512; physical++) {
        stuck = 0;
        selected = 2; /* Includes starting with a column still driven. */
        matrix_sample_t sample = matrix_scan_cycle(&io);
        assert(sample.keys == physical);
        assert(sample.invalid_rows == 0);
        assert(selected == -1);
    }
    for (stuck = 1; stuck < 8; stuck++) {
        physical = 0x1ff;
        uint16_t expected = physical;
        for (unsigned r = 0; r < 3; r++)
            if (stuck & (1U << r)) expected &= ~(7U << (r * 3));
        matrix_sample_t sample = matrix_scan_cycle(&io);
        assert(sample.invalid_rows == stuck);
        assert(sample.keys == expected);
    }
    stuck = 4; physical = 0;
    assert(matrix_scan_cycle(&io).keys == 0); /* D2 LOW must not become 7/8/9. */
    stuck = 0; physical = 1U << 6;
    assert(matrix_scan_cycle(&io).keys == (1U << 6)); /* Real 7 survives recovery. */
    puts("Matrix OK: 512 key combinations, stuck rows, settling and recovery.");
}
