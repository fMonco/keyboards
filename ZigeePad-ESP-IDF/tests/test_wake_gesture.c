#include <assert.h>
#include <stdio.h>
#include "wake_gesture.h"

static void check(unsigned key, unsigned second_start, unsigned second_end,
                  unsigned expected)
{
    wake_gesture_t capture = {0};
    unsigned end = 0;
    for (unsigned t = 0; t <= 2000; t += 5) {
        bool down = t < 100 || (t >= second_start && t < second_end);
        if (wake_gesture_step(&capture, down ? 1U << key : 0, 0, t)) {
            end = t;
            break;
        }
    }
    assert(end && capture.count == 1);
    assert(capture.events[0] == key + 1 + expected);
    // Both clicks are over BEFORE boot. The app sees only released pins.
    button_state_t app[9] = {0};
    wake_gesture_restore(&capture, app, 250);
    for (unsigned t = 250; t < 1500; t += 5)
        assert(button_step(&app[key], false, t) == BUTTON_NONE);
}

int main(void)
{
    for (unsigned key = 0; key < 9; key++) {
        check(key, 150, 230, 10); // Fast double entirely before normal boot finishes.
        check(key, 600, 700, 10); // Full 500 ms inter-click window.
        check(key, 3000, 3100, 0); // Single once; no replay after boot.
    }
    wake_gesture_t capture = {0};
    for (unsigned t = 0; t <= 700; t += 5)
        assert(wake_gesture_step(&capture, 2, 0, t) == (t == 700));
    assert(capture.count == 1 && capture.events[0] == 22);
    button_state_t app[9];
    wake_gesture_restore(&capture, app, 100);
    for (unsigned t = 100; t < 2000; t += 5)
        assert(button_step(&app[1], t < 1000, t) == BUTTON_NONE);
    capture = (wake_gesture_t){0};
    assert(wake_gesture_step(&capture, 5, 0, 0));
    assert(capture.count == 0); // Reset chord is handled by app, not sent as holds.
    capture = (wake_gesture_t){0};
    assert(wake_gesture_step(&capture, 257, 0, 0));
    assert(capture.count == 0);
    capture = (wake_gesture_t){0};
    assert(!wake_gesture_step(&capture, 0, 1, 100));
    assert(wake_gesture_step(&capture, 0, 1, 2000));
    assert(capture.count == 0); // Invalid row cannot create a click or stall boot forever.
    puts("Wake gesture tests passed (all 9 keys, double/single/hold, handoff, chords, invalid row)");
}
