#include "buttons.h"

button_action_t button_update(button_state_t *b, bool raw, uint32_t now)
{
    if (raw != b->raw) { b->raw = raw; b->changed_at = now; }
    if (b->waiting && !b->down && b->raw &&
        b->changed_at - b->released_at > BUTTON_DOUBLE_MS) {
        b->waiting = false;
        return BUTTON_SINGLE;
    }
    /* Measure physical release time, not the end of release debounce. */
    uint32_t press_end = b->raw ? now : b->changed_at;
    if (b->down && !b->held && press_end - b->pressed_at >= BUTTON_HOLD_MS) {
        if (b->second && b->waiting) {
            b->waiting = b->second = false;
            return BUTTON_SINGLE;
        }
        b->held = true;
        return BUTTON_HOLD;
    }
    if (b->raw != b->down && now - b->changed_at >= BUTTON_DEBOUNCE_MS) {
        b->down = b->raw;
        if (b->down) {
            b->pressed_at = b->changed_at;
            b->held = false;
            b->second = b->waiting && b->pressed_at - b->released_at <= BUTTON_DOUBLE_MS;
        } else if (!b->held) {
            if (b->second) {
                b->waiting = b->second = false;
                return BUTTON_DOUBLE;
            }
            b->waiting = true;
            b->released_at = b->changed_at;
        }
    }
    /* A second press must have time to pass debounce at the window boundary. */
    if (b->waiting && !b->down && !b->raw && now - b->released_at > BUTTON_DOUBLE_MS) {
        b->waiting = false;
        return BUTTON_SINGLE;
    }
    return BUTTON_NONE;
}

bool button_busy(const button_state_t *b)
{
    return b->raw || b->down || b->waiting;
}
