#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Настройки жестов, все значения в миллисекундах. */
#define BUTTON_DEBOUNCE_MS 25  // Уровень должен быть стабильным, чтобы принять изменение.
#define BUTTON_DOUBLE_MS 350  // От отпускания первого до начала второго нажатия.
#define BUTTON_HOLD_MS 700    // Hold один раз после удержания; отпускание не даёт single.
typedef enum { BUTTON_NONE, BUTTON_SINGLE, BUTTON_DOUBLE, BUTTON_HOLD } button_action_t;
typedef struct {
    // raw: физический уровень; down: после debounce; held: hold уже отправлен;
    // waiting: ожидаем второй клик; second: текущее нажатие — кандидат в double.
    bool raw, down, held, waiting, second;
    uint32_t changed_at, pressed_at, released_at;
} button_state_t;
button_action_t button_update(button_state_t *b, bool raw, uint32_t now);
bool button_busy(const button_state_t *b); // Пока жест не завершён, deep sleep запрещён.
