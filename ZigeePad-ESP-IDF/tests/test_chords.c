#include <assert.h>
#include <stdio.h>
#include "chords.h"

static void check_chord(unsigned mask, unsigned duration, chord_action_t expected, uint32_t base)
{
    chord_state_t s = {0};
    assert(chord_update(&s, mask, false, base) == CHORD_NONE);
    assert(s.suppress);
    chord_update(&s, mask, false, base + duration - 1);
    assert(s.armed == CHORD_NONE);
    assert(chord_update(&s, mask, false, base + duration) == CHORD_NONE);
    assert(s.armed == expected);
    assert(chord_update(&s, 1, false, base + duration + 10) == CHORD_NONE);
    assert(chord_update(&s, 0, false, base + duration + 20) == CHORD_NONE);
    assert(chord_update(&s, 0, false, base + duration + 44) == CHORD_NONE);
    assert(chord_update(&s, 0, false, base + duration + 45) == expected);
    assert(!s.suppress);
    assert(chord_update(&s, 0, false, base + duration + 100) == CHORD_NONE);
}

int main(void)
{
    check_chord(5, 3000, CHORD_RESTART, 100);
    check_chord(257, 5000, CHORD_FACTORY, 100);
    check_chord(257, 5000, CHORD_FACTORY, UINT32_MAX - 2000);
    chord_state_t s = {0};
    chord_update(&s, 1, false, 0);
    assert(!s.suppress); // Individual key keeps normal gestures.
    chord_update(&s, 261, false, 100);
    chord_update(&s, 261, false, 10000);
    assert(!s.suppress && s.armed == CHORD_NONE); // Both chords together do nothing.
    chord_update(&s, 257, false, 11000);
    chord_update(&s, 0, false, 12000);
    assert(chord_update(&s, 0, false, 12025) == CHORD_NONE && !s.suppress);
    chord_update(&s, 257, false, 13000);
    chord_update(&s, 1, false, 17000); // Incomplete chord restarts hold timing.
    chord_update(&s, 257, false, 18000);
    chord_update(&s, 257, false, 22000);
    assert(s.armed == CHORD_NONE);
    chord_update(&s, 257, false, 23000);
    assert(s.armed == CHORD_FACTORY);
    chord_update(&s, 0, true, 23001); // Electrical error cancels even armed reset.
    chord_update(&s, 0, false, 23002);
    assert(chord_update(&s, 0, false, 23027) == CHORD_NONE);
    puts("Chord tests passed");
}
