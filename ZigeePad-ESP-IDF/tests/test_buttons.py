"""Run the actual portable C state machine via a host DLL, not a Python copy.

Build DLL with a Windows host C compiler as described in README.md, then:
    python tests/test_buttons.py build/buttons.dll
"""
import ctypes as C
import sys
import unittest
from pathlib import Path

class State(C.Structure):
    _fields_ = [(k, C.c_bool) for k in ('raw', 'down', 'held', 'waiting', 'second')] + [
        (k, C.c_uint32) for k in ('changed_at', 'pressed_at', 'released_at')]

lib = C.CDLL(str(Path(sys.argv.pop(1)).resolve()))
lib.button_update.argtypes = [C.POINTER(State), C.c_bool, C.c_uint32]
lib.button_update.restype = C.c_int
lib.button_busy.argtypes = [C.POINTER(State)]
lib.button_busy.restype = C.c_bool

def simulate(intervals, duration=2000, offset=0):
    state = State()
    events = []
    for t in range(0, duration, 5):
        raw = any(a <= t < b for a, b in intervals)
        event = lib.button_update(C.byref(state), raw, (t + offset) & 0xffffffff)
        if event:
            events.append(event)
    return events, lib.button_busy(C.byref(state))

class ButtonsTest(unittest.TestCase):
    def test_single(self):
        self.assertEqual(simulate([(100, 200)]), ([1], False))

    def test_double_no_single(self):
        self.assertEqual(simulate([(100, 200), (300, 400)]), ([2], False))

    def test_two_doubles_same_key(self):
        for gap in (30, 100, 300, 600, 1200):
            with self.subTest(gap=gap):
                start = 400 + gap
                self.assertEqual(simulate([(100, 200), (300, 400),
                                           (start, start + 100), (start + 200, start + 300)],
                                          duration=start + 1000), ([2, 2], False))

    def test_repeated_doubles_same_key(self):
        intervals = [(100 + i * 200, 200 + i * 200) for i in range(20)]
        self.assertEqual(simulate(intervals, duration=5000), ([2] * 10, False))

    def test_hold_once_no_release_event(self):
        self.assertEqual(simulate([(100, 1600)]), ([3], False))

    def test_bounce(self):
        self.assertEqual(simulate([(100, 110), (115, 120), (125, 250), (260, 270)]), ([1], False))

    def test_noise_ignored(self):
        self.assertEqual(simulate([(100, 110), (120, 130)]), ([], False))

    def test_double_boundary_includes_debounce(self):
        self.assertEqual(simulate([(100, 200), (700, 800)]), ([2], False))

    def test_late_second_is_two_singles(self):
        self.assertEqual(simulate([(100, 200), (705, 800)]), ([1, 1], False))

    def test_tap_then_hold(self):
        self.assertEqual(simulate([(100, 200), (300, 1200)]), ([1, 3], False))

    def test_held_key_prevents_sleep(self):
        self.assertEqual(simulate([(100, 10000)]), ([3], True))

    def test_counter_wrap(self):
        self.assertEqual(simulate([(100, 200), (300, 400)], offset=0xffffff00), ([2], False))

    def test_release_just_before_hold(self):
        self.assertEqual(simulate([(100, 795)]), ([1], False))

    def test_release_at_hold_threshold(self):
        self.assertEqual(simulate([(100, 800)]), ([3], False))

    def test_triple_tap(self):
        self.assertEqual(simulate([(100, 200), (300, 400), (500, 600)]), ([2, 1], False))

    def test_wake_capture_released_during_boot(self):
        state = State(down=True, pressed_at=0, changed_at=0)
        events = [lib.button_update(C.byref(state), False, t) for t in range(0, 1000, 5)]
        self.assertEqual([e for e in events if e], [1])

    def test_independent_keys(self):
        states = [State() for _ in range(9)]
        output = []
        for t in range(0, 2000, 5):
            for i, state in enumerate(states):
                event = lib.button_update(C.byref(state), 100 <= t < 200 + i * 100, t)
                if event: output.append((i, event))
        self.assertEqual(sorted(output), [(i, 1 if i < 6 else 3) for i in range(9)])

if __name__ == '__main__':
    unittest.main()
