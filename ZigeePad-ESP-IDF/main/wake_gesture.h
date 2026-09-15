#pragma once
#include "button_logic.h"

#define WAKE_GESTURE_LIMIT_MS 2000U
#define WAKE_EVENT_CAPACITY 18U
typedef struct {
    button_state_t buttons[9];
    uint16_t events[WAKE_EVENT_CAPACITY];
    uint32_t elapsed;
    uint8_t count;
    bool valid;
} wake_gesture_t;

/* Continue scanning until the initial gesture is resolved, instead of
 * booting between the two clicks. A held key or reset chord hands off to
 * the app, with button state preserved to avoid a duplicate hold/press. */
static inline bool wake_gesture_step(wake_gesture_t *capture, uint16_t raw,
                                     uint8_t invalid_rows, uint32_t now)
{
    capture->elapsed = now;
    capture->valid = true;
    if (!invalid_rows && (raw == 5U || raw == 257U)) return true;
    bool unresolved = invalid_rows != 0;
    for (unsigned i = 0; i < 9; i++) {
        button_state_t *b = &capture->buttons[i];
        if (invalid_rows & (1U << (i / 3))) {
            b->raw = b->down = b->held = b->waiting = b->second = false;
            continue;
        }
        button_action_t action = button_step(b, (raw >> i) & 1U, now);
        if (action != BUTTON_NONE && capture->count < WAKE_EVENT_CAPACITY)
            capture->events[capture->count++] = i + 1 + (action - 1) * 10;
        unresolved |= b->waiting || b->raw != b->down || (b->down && !b->held);
    }
    return (!unresolved && now >= BUTTON_DEBOUNCE_MS) || now >= WAKE_GESTURE_LIMIT_MS;
}

static inline void wake_gesture_restore(const wake_gesture_t *capture,
                                       button_state_t *buttons, uint32_t app_now)
{
    uint32_t shift = app_now - capture->elapsed;
    for (unsigned i = 0; i < 9; i++) {
        buttons[i] = capture->buttons[i];
        buttons[i].changed_at += shift;
        buttons[i].pressed_at += shift;
        buttons[i].released_at += shift;
    }
}
