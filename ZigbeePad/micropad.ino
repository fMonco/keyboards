#include <Arduino.h>
#include "Zigbee.h"

#include "nvs_flash.h"
#include "esp_zigbee_core.h"

#ifndef ZIGBEE_MODE_ED
#error "Tools -> Zigbee Mode -> Zigbee ED (end device)"
#endif

#define ZB_ENDPOINT 1

ZigbeeMultistate zbPad(ZB_ENDPOINT);

// ============================================================
// Matrix
// ============================================================

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

// ============================================================
// Timings
// ============================================================

const uint32_t DEBOUNCE_MS      = 25;
const uint32_t DOUBLE_MS        = 350;
const uint32_t HOLD_MS          = 700;

const uint32_t REBOOT_MS        = 5000;
const uint32_t ZIGBEE_RESET_MS  = 10000;
const uint32_t FULL_RESET_MS    = 15000;

// ============================================================
// Key state
// ============================================================

struct KeyState {
  bool raw = false;
  bool stable = false;

  uint32_t rawChangedAt = 0;
  uint32_t pressedAt = 0;
  uint32_t releasedAt = 0;

  uint8_t clickCount = 0;
  bool holdSent = false;
};

KeyState keys[10];

// ============================================================
// Combo state
// ============================================================

enum ComboType {
  COMBO_NONE,
  COMBO_REBOOT,
  COMBO_ZIGBEE_RESET,
  COMBO_FULL_RESET
};

ComboType activeCombo = COMBO_NONE;
uint32_t comboStartedAt = 0;
bool comboTriggered = false;

// ============================================================
// Matrix scan
// ============================================================

void scanMatrix(bool result[10]) {
  for (int i = 0; i < 10; i++) {
    result[i] = false;
  }

  for (int c = 0; c < 3; c++) {

    for (int i = 0; i < 3; i++) {
      pinMode(colPins[i], INPUT);
    }

    pinMode(colPins[c], OUTPUT);
    digitalWrite(colPins[c], LOW);

    delayMicroseconds(50);

    for (int r = 0; r < 3; r++) {
      if (digitalRead(rowPins[r]) == LOW) {
        result[keyMap[r][c]] = true;
      }
    }

    pinMode(colPins[c], INPUT);
  }
}

// ============================================================
// Direct Zigbee report
// ============================================================

bool reportMicropadDirect() {
  esp_zb_zcl_report_attr_cmd_t cmd = {};

  cmd.address_mode =
    ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;

  // Zigbee coordinator short address
  cmd.zcl_basic_cmd.dst_addr_u.addr_short = 0x0000;

  // Coordinator endpoint
  cmd.zcl_basic_cmd.dst_endpoint = 1;

  // Micropad endpoint
  cmd.zcl_basic_cmd.src_endpoint = ZB_ENDPOINT;

  cmd.clusterID =
    ESP_ZB_ZCL_CLUSTER_ID_MULTI_INPUT;

  cmd.attributeID =
    ESP_ZB_ZCL_ATTR_MULTI_INPUT_PRESENT_VALUE_ID;

  cmd.direction =
    ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;

  cmd.manuf_specific = 0;
  cmd.dis_default_resp = 0;

  esp_zb_lock_acquire(portMAX_DELAY);

  esp_err_t err =
    esp_zb_zcl_report_attr_cmd_req(&cmd);

  esp_zb_lock_release();

  if (err != ESP_OK) {
    Serial.printf(
      "Direct report FAILED: %s\n",
      esp_err_to_name(err)
    );
    return false;
  }

  return true;
}

// ============================================================
// Zigbee events
//
//  1..9   = single
// 11..19  = double
// 21..29  = hold
// ============================================================

void sendZigbeeValue(uint16_t value) {
  if (!Zigbee.connected()) {
    Serial.println("Zigbee not connected, event dropped");
    return;
  }

  Serial.printf("Zigbee value: %u\n", value);

  bool setOk = zbPad.setMultistateInput(value);

  if (!setOk) {
    Serial.println("setMultistateInput FAILED");
    return;
  }

  if (!reportMicropadDirect()) {
    return;
  }

  delay(30);

  // Reset to zero so identical actions can be sent repeatedly.
  zbPad.setMultistateInput(0);
  reportMicropadDirect();
}

void sendSingle(uint8_t key) {
  Serial.printf("BUTTON %u SINGLE\n", key);
  sendZigbeeValue(key);
}

void sendDouble(uint8_t key) {
  Serial.printf("BUTTON %u DOUBLE\n", key);
  sendZigbeeValue(10 + key);
}

void sendHold(uint8_t key) {
  Serial.printf("BUTTON %u HOLD\n", key);
  sendZigbeeValue(20 + key);
}

// ============================================================
// Utility
// ============================================================

bool keyDown(uint8_t key) {
  return keys[key].stable;
}

bool keyBelongsToActiveCombo(uint8_t key) {
  switch (activeCombo) {

    case COMBO_REBOOT:
      return key == 1 || key == 3;

    case COMBO_ZIGBEE_RESET:
      return key == 7 || key == 9;

    case COMBO_FULL_RESET:
      return key == 1 || key == 9;

    default:
      return false;
  }
}

void clearKeyEvents(uint8_t key) {
  keys[key].clickCount = 0;
  keys[key].holdSent = true;
}

// ============================================================
// Reset actions
// ============================================================

void doReboot() {
  Serial.println();
  Serial.println("=== REBOOT ===");
  Serial.flush();

  delay(300);
  ESP.restart();
}

void doZigbeeFactoryReset() {
  Serial.println();
  Serial.println("=== ZIGBEE FACTORY RESET ===");
  Serial.println("Zigbee network will be forgotten.");
  Serial.flush();

  delay(300);

  Zigbee.factoryReset();
}

void doFullFactoryReset() {
  Serial.println();
  Serial.println("=== FULL ESP32 FACTORY RESET ===");
  Serial.println("Clearing NVS + Zigbee network.");
  Serial.flush();

  esp_err_t err = nvs_flash_erase();

  Serial.printf(
    "nvs_flash_erase(): %s\n",
    esp_err_to_name(err)
  );

  Serial.flush();
  delay(300);

  Zigbee.factoryReset();
}

// ============================================================
// Reset combos
// ============================================================

void updateCombo() {
  ComboType detected = COMBO_NONE;
  uint32_t requiredTime = 0;

  // Most destructive combo first.
  if (keyDown(1) && keyDown(9)) {
    detected = COMBO_FULL_RESET;
    requiredTime = FULL_RESET_MS;
  }
  else if (keyDown(7) && keyDown(9)) {
    detected = COMBO_ZIGBEE_RESET;
    requiredTime = ZIGBEE_RESET_MS;
  }
  else if (keyDown(1) && keyDown(3)) {
    detected = COMBO_REBOOT;
    requiredTime = REBOOT_MS;
  }

  if (detected == COMBO_NONE) {
    activeCombo = COMBO_NONE;
    comboStartedAt = 0;
    comboTriggered = false;
    return;
  }

  if (detected != activeCombo) {
    activeCombo = detected;
    comboStartedAt = millis();
    comboTriggered = false;

    switch (activeCombo) {

      case COMBO_REBOOT:
        clearKeyEvents(1);
        clearKeyEvents(3);
        Serial.println("Combo 1+3 detected...");
        break;

      case COMBO_ZIGBEE_RESET:
        clearKeyEvents(7);
        clearKeyEvents(9);
        Serial.println("Combo 7+9 detected...");
        break;

      case COMBO_FULL_RESET:
        clearKeyEvents(1);
        clearKeyEvents(9);
        Serial.println("Combo 1+9 detected...");
        break;

      default:
        break;
    }

    return;
  }

  if (
    !comboTriggered &&
    millis() - comboStartedAt >= requiredTime
  ) {
    comboTriggered = true;

    switch (activeCombo) {

      case COMBO_REBOOT:
        doReboot();
        break;

      case COMBO_ZIGBEE_RESET:
        doZigbeeFactoryReset();
        break;

      case COMBO_FULL_RESET:
        doFullFactoryReset();
        break;

      default:
        break;
    }
  }
}

// ============================================================
// Button state machine
// ============================================================

void updateButtons() {
  bool scan[10];
  scanMatrix(scan);

  uint32_t now = millis();

  // Debounce
  for (uint8_t key = 1; key <= 9; key++) {

    if (scan[key] != keys[key].raw) {
      keys[key].raw = scan[key];
      keys[key].rawChangedAt = now;
    }

    if (
      keys[key].stable != keys[key].raw &&
      now - keys[key].rawChangedAt >= DEBOUNCE_MS
    ) {
      keys[key].stable = keys[key].raw;

      if (keys[key].stable) {
        // Pressed
        keys[key].pressedAt = now;
        keys[key].holdSent = false;

      } else {
        // Released
        keys[key].releasedAt = now;

        if (
          !keys[key].holdSent &&
          !keyBelongsToActiveCombo(key)
        ) {
          keys[key].clickCount++;

          if (keys[key].clickCount >= 2) {
            sendDouble(key);
            keys[key].clickCount = 0;
          }
        }
      }
    }
  }

  // Service combos before normal hold handling.
  updateCombo();

  // Hold
  for (uint8_t key = 1; key <= 9; key++) {

    if (
      keys[key].stable &&
      !keys[key].holdSent &&
      !keyBelongsToActiveCombo(key) &&
      now - keys[key].pressedAt >= HOLD_MS
    ) {
      keys[key].holdSent = true;
      keys[key].clickCount = 0;

      sendHold(key);
    }
  }

  // Single after waiting for possible second click.
  for (uint8_t key = 1; key <= 9; key++) {

    if (
      !keys[key].stable &&
      keys[key].clickCount == 1 &&
      now - keys[key].releasedAt >= DOUBLE_MS
    ) {
      keys[key].clickCount = 0;

      if (!keyBelongsToActiveCombo(key)) {
        sendSingle(key);
      }
    }
  }
}

// ============================================================
// Setup
// ============================================================

void setup() {
  Serial.begin(115200);

  Serial.println();
  Serial.println("============================");
  Serial.println("        Monco Micropad");
  Serial.println("============================");

  // Matrix
  for (int r = 0; r < 3; r++) {
    pinMode(rowPins[r], INPUT_PULLUP);
  }

  for (int c = 0; c < 3; c++) {
    pinMode(colPins[c], INPUT);
  }

  // Zigbee endpoint
  zbPad.setManufacturerAndModel(
    "Monco",
    "Micropad"
  );

  zbPad.addMultistateInput();

  zbPad.setMultistateInputApplication(
    ZB_MULTISTATE_APPLICATION_TYPE_OTHER_INDEX
  );

  zbPad.setMultistateInputDescription(
    "Micropad action"
  );

  /*
    presentValue map:

    0       idle

    1..9    single
    11..19  double
    21..29  hold
  */
  zbPad.setMultistateInputStates(30);

  Zigbee.addEndpoint(&zbPad);

  Serial.println("Starting Zigbee...");

  if (!Zigbee.begin()) {
    Serial.println("Zigbee start failed!");
    delay(1000);
    ESP.restart();
  }

  Serial.print("Connecting");

  while (!Zigbee.connected()) {
    Serial.print(".");
    delay(500);
  }

  Serial.println();
  Serial.println("Zigbee connected!");
  Serial.println();
}

// ============================================================
// Main loop
// ============================================================

void loop() {
  updateButtons();
  delay(5);
}