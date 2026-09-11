#include "chords.h"
#include "buttons.h"
#include <string.h>

chord_action_t chord_update(chord_state_t *s, uint16_t keys, bool invalid, uint32_t now)
{
    const uint16_t restart = (1U << 0) | (1U << 2);
    const uint16_t factory = (1U << 0) | (1U << 8);
    if (invalid) {
        s->candidate = 0;
        s->armed = CHORD_NONE;
        s->releasing = false;
        return CHORD_NONE;
    }
    if (s->suppress && !keys) {
        if (!s->releasing) { s->releasing = true; s->released_at = now; }
        if (now - s->released_at >= BUTTON_DEBOUNCE_MS) {
            chord_action_t action = s->armed;
            memset(s, 0, sizeof(*s));
            return action;
        }
        return CHORD_NONE;
    }
    s->releasing = false;
    if (s->armed != CHORD_NONE) return CHORD_NONE;
    uint16_t candidate = (keys == restart || keys == factory) ? keys : 0;
    if (candidate != s->candidate) { s->candidate = candidate; s->since = now; }
    if (candidate) {
        s->suppress = true;
        uint32_t duration = candidate == restart ? CHORD_RESTART_MS : CHORD_FACTORY_MS;
        if (now - s->since >= duration)
            s->armed = candidate == restart ? CHORD_RESTART : CHORD_FACTORY;
    }
    return CHORD_NONE;
}
