#pragma once
#include <stdbool.h>
#include <stdint.h>
#define CHORD_RESTART_MS 3000U
#define CHORD_FACTORY_MS 5000U
typedef enum { CHORD_NONE, CHORD_RESTART, CHORD_FACTORY } chord_action_t;
typedef struct {
    uint16_t candidate;
    uint32_t since, released_at;
    bool suppress, releasing;
    chord_action_t armed;
} chord_state_t;
chord_action_t chord_update(chord_state_t *s, uint16_t keys, bool invalid, uint32_t now);
