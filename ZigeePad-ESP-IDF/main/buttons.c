#include "button_logic.h"

button_action_t button_update(button_state_t *b, bool raw, uint32_t now)
{
    return button_step(b, raw, now);
}

bool button_busy(const button_state_t *b)
{
    return b->raw || b->down || b->waiting;
}
