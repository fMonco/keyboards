### Used repos:
https://github.com/StaRky33/ZigbeeMacropad

# Monco Micropad

Самодельный батарейный Zigbee-макропад 3×3 для Home Assistant / Zigbee2MQTT.

## Hardware

- Seeed Studio XIAO ESP32-C6
- 9× mechanical switches
- 9× 1N4148
- LiPo 3.7 V 400 mAh
  - использован аккумулятор 502535
  - размеры примерно 5×25×35 мм
- корпус: VOID9
  - https://www.printables.com/model/347529-void9-macropad

ESP32-C6 выбран из-за встроенного IEEE 802.15.4 / Zigbee.

Аккумулятор подключается напрямую к:

- красный → BAT+
- чёрный → BAT-

Зарядка аккумулятора идёт через USB-C XIAO.

---

## Нумерация кнопок

Если смотреть на макропад сверху:

    ┌───┬───┬───┐
    │ 7 │ 8 │ 9 │
    ├───┼───┼───┤
    │ 4 │ 5 │ 6 │
    ├───┼───┼───┤
    │ 1 │ 2 │ 3 │
    └───┴───┴───┘

---

## Матрица

Фактическая текущая разводка:

### Rows

    9 8 7 → D2
    6 5 4 → D3
    3 2 1 → D6

### Columns

    9 6 3 → D10
    8 5 2 → D9
    7 4 1 → D0

Итого:

             D10     D9      D0
              │       │       │
    D2       [9]     [8]     [7]
    D3       [6]     [5]     [4]
    D6       [3]     [2]     [1]

На каждой клавише установлен отдельный 1N4148.

Все диоды должны быть ориентированы одинаково.

> Важно: при изменении прошивки сверять `rowPins[]` и `colPins[]`
> именно с этой физической разводкой.

В текущей прошивке это:

    const uint8_t rowPins[3] = {
      D2,  // 9 8 7
      D3,  // 6 5 4
      D6   // 3 2 1
    };

    const uint8_t colPins[3] = {
      D10, // 9 6 3
      D9,  // 8 5 2
      D0   // 7 4 1
    };

    const uint8_t keyMap[3][3] = {
      {9, 8, 7},
      {6, 5, 4},
      {3, 2, 1}
    };
    
---

## Zigbee

Устройство работает как Zigbee End Device.

Manufacturer:

    Monco

Model ID:

    Micropad

Zigbee endpoint использует `genMultistateInput`.

Прошивка отправляет `presentValue`:

    0       = idle

    1..9    = обычное нажатие
    11..19  = double click
    21..29  = hold

Например:

    5  → Button 5 press
    15 → Button 5 double
    25 → Button 5 hold

---

## Zigbee2MQTT

Используется external converter:

    zigbee2mqtt/external_converters/micropad.mjs

В `configuration.yaml` должно быть:

    advanced:
      enable_external_js: true

После изменения converter:

    docker compose restart zigbee2mqtt

Проверить загрузку:

    docker compose logs zigbee2mqtt | grep "Loaded external converter"

Должно быть:

    Loaded external converter 'micropad.mjs'

---

## Actions

External converter преобразует Zigbee `presentValue` в MQTT actions:

    button_1_press
    button_1_double
    button_1_hold

    button_2_press
    button_2_double
    button_2_hold

    ...

    button_9_press
    button_9_double
    button_9_hold

MQTT topic:

    zigbee2mqtt/Micropad/action

Проверка:

    docker exec -it mosquitto \
      mosquitto_sub \
      -t 'zigbee2mqtt/Micropad/action' \
      -v

Пример:

    zigbee2mqtt/Micropad/action button_5_press

---

## Home Assistant

Zigbee2MQTT публикует actions через MQTT Discovery.

Для автоматизаций можно использовать MQTT напрямую:

    triggers:
      - trigger: mqtt
        topic: zigbee2mqtt/Micropad/action
        payload: button_5_press

Либо использовать MQTT Device Triggers, если они появились через discovery.

Для всех кнопок удобно сделать одну HA automation и внутри использовать `choose`
по значению `trigger.payload`.

---

## Button logic

Поддерживаются:

- press
- double
- hold

Тайминги:

    debounce: 25 ms
    double:   350 ms
    hold:     700 ms

---

## Service combinations

### Reboot

    1 + 3
    держать 5 секунд

Выполняет:

    ESP.restart()

Настройки Zigbee при этом сохраняются.

### Zigbee factory reset

    7 + 9
    держать 10 секунд

Устройство забывает текущую Zigbee-сеть и после этого его нужно заново
подключить к Zigbee2MQTT.

### Full factory reset

    1 + 9
    держать 15 секунд

Выполняется:

    nvs_flash_erase()
    Zigbee.factoryReset()

Очищается NVS и Zigbee network state.

Сама прошивка из flash НЕ удаляется.

---

## Recovery / прошивка

Для обычной прошивки используется Arduino IDE.

Board:

    XIAO_ESP32C6

Zigbee mode:

    Zigbee ED (End Device)

USB:

    USB CDC On Boot → Enabled

Если загружена сломанная прошивка или ESP ушёл в sleep и USB больше
нормально не работает:

1. зажать BOOT
2. нажать RESET
3. отпустить RESET
4. отпустить BOOT
5. перепрошить через Arduino IDE

Поэтому физические BOOT и RESET на XIAO лучше оставить доступными через
отверстия снизу корпуса.

---

## Power saving

Изначально тестировался light sleep, но во время отладки он был полностью
отключён.

Текущая стабильная версия работает без sleep:

    void loop() {
        updateButtons();
        delay(5);
    }

Когда вся логика окончательно отлажена, можно отдельно вернуть deep/light
sleep и пробуждение от матрицы.

Для LiPo 400 mAh ожидаемая автономность после нормальной оптимизации sleep —
много месяцев.

---

## Files

Рекомендуемая структура репозитория:

    micropad/
    ├── README.md
    ├── firmware/
    │   └── micropad.ino
    └── zigbee2mqtt/
        └── micropad.mjs

Прошивку и Zigbee2MQTT converter желательно хранить вместе, чтобы их версии
не разъехались.
