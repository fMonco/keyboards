#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "esp_zigbee.h"
#include "ezbee/zcl/cluster/basic.h"
#include "ezbee/zcl/cluster/identify.h"
#include "ezbee/zcl/cluster/multistate_input.h"
#include "buttons.h"
#include "chords.h"
#include "esp_system.h"
#include "wake_capture.h"
#include "board_pins.h"
#include "matrix_scan.h"

/* Zigbee: endpoint 1, кластер 0x0012, атрибут presentValue 0x0055.
 * Формат событий должен совпадать с zigbee2mqtt/micropad.mjs. */
#define EP 1
#define CLUSTER EZB_ZCL_CLUSTER_ID_MULTISTATE_INPUT
#define VALUE_ATTR EZB_ZCL_ATTR_MULTISTATE_INPUT_PRESENT_VALUE_ID
/* Все интервалы ниже — миллисекунды. Это условия разрешения сна,
 * а не фиксированное время работы: передача и удерживаемая кнопка продляют его. */
#define IDLE_MS 2000U                 // Пауза после завершения жеста/активности.
#define SETUP_AWAKE_MS 20000U        // Окно настройки после подключения при RESET/первом join.
#define RECONNECT_WINDOW_MS 25000U  // Нет сети после пробуждения клавишей: ждём до 25 с.
#define READY_SETTLE_MS 1000U        // Минимум 1 с после восстановления Zigbee при key wake.
#define TX_SETTLE_MS 250U           // Минимум 250 мс после последнего результата TX.
#define JOIN_WINDOW_MS 120000U      // Нет сети после RESET/питания: окно 120 с.
#define TX_TIMEOUT_MS 15000U        // Нет подтверждения TX за 15 с: сохранить событие до wake.
#define RETRY_MS 2000U              // Пауза между попытками подключения/неудачными TX.
#define READY BIT0
#define INITIALIZED BIT1
#define INTERVIEW BIT2
#define QUEUE_SIZE 32               // Ёмкость очереди событий в RTC RAM.
#define STATUS_LED_GPIO GPIO_NUM_15 // Встроенный LED XIAO: LOW = включён.
#define LED_BLINK_MS 80U            // Вспышка 80 мс, пауза 80 мс, затем постоянный свет.
_Static_assert(TX_SETTLE_MS >= 2 * LED_BLINK_MS, "TX settling must allow the LED blink to finish");

static const char *TAG = "Micropad";
/* Строки D0/D1/D2 — входы пробуждения. При их переносе также менять
 * row_cfg, маску EXT1 в enter_sleep() и wake_rows/hold-маску в wake stub.
 * Столбцы D7/D8/D9 задаются общим файлом board_pins.h. */
static const gpio_num_t rows[] = {GPIO_NUM_0, GPIO_NUM_1, GPIO_NUM_2};
static const gpio_num_t cols[] = MATRIX_COL_GPIOS;
static button_state_t buttons[9];
static uint8_t matrix_invalid_rows;
static EventGroupHandle_t network;
static QueueHandle_t confirmations;
/* Accessed exclusively with Zigbee lock held or from stack callbacks. */
static uint32_t retry_mode, retry_at;
static bool retry_pending;
static bool led_has_confirmation;
static uint32_t led_confirmed_at;
static uint16_t led_chord_candidate;
static bool led_chord_armed;

/* Only the application task owns this ring. Retained across deep sleep,
 * not across removal of power. Never replay an already confirmed event. */
static RTC_DATA_ATTR struct {
    uint16_t values[QUEUE_SIZE];
    uint8_t head, count;
} pending;
typedef struct { uint32_t token; uint8_t status; } tx_result_t;
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void status_led_init(void)
{
    /* Сначала задаём выключенный уровень, затем снимаем удержание после сна. */
    ESP_ERROR_CHECK(gpio_set_level(STATUS_LED_GPIO, 1));
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << STATUS_LED_GPIO, .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    ESP_ERROR_CHECK(gpio_hold_dis(STATUS_LED_GPIO));
}

static void status_led_update(uint32_t now)
{
    /* Неблокирующая индикация: обработка кнопок и Zigbee продолжается.
     * Каждый успешный TX начинает новый блинк; ошибки его не запускают. */
    uint32_t elapsed = now - led_confirmed_at;
    bool on = led_has_confirmation && (elapsed < LED_BLINK_MS || elapsed >= 2 * LED_BLINK_MS);
    // Служебное сочетание имеет приоритет над индикацией Zigbee.
    if (led_chord_armed) on = true; // Порог достигнут: отпустить все кнопки.
    else if (led_chord_candidate) {
        uint32_t half_period = led_chord_candidate == 5 ? 250U : 100U;
        on = (now / half_period) % 2 == 0;
    }
    gpio_set_level(STATUS_LED_GPIO, on ? 0 : 1);
}

static void matrix_init(void)
{
    for (unsigned r = 0; r < 3; r++) {
        ESP_ERROR_CHECK(rtc_gpio_hold_dis(rows[r]));
        ESP_ERROR_CHECK(rtc_gpio_deinit(rows[r]));
    }
    const gpio_config_t row_cfg = {
        .pin_bit_mask = (1ULL << GPIO_NUM_0) | (1ULL << GPIO_NUM_1) | (1ULL << GPIO_NUM_2),
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    const gpio_config_t col_cfg = {
        .pin_bit_mask = MATRIX_COL_MASK, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&row_cfg));
    ESP_ERROR_CHECK(gpio_config(&col_cfg));
    for (unsigned c = 0; c < 3; c++) {
        ESP_ERROR_CHECK(gpio_set_level(cols[c], 0));
        ESP_ERROR_CHECK(gpio_set_direction(cols[c], GPIO_MODE_INPUT));
        ESP_ERROR_CHECK(gpio_hold_dis(cols[c]));
        ESP_ERROR_CHECK(gpio_set_pull_mode(cols[c], GPIO_PULLUP_ONLY));
    }
    for (unsigned r = 0; r < 3; r++) {
        ESP_ERROR_CHECK(gpio_hold_dis(rows[r]));
        ESP_ERROR_CHECK(gpio_set_direction(rows[r], GPIO_MODE_INPUT));
        ESP_ERROR_CHECK(gpio_set_pull_mode(rows[r], GPIO_PULLUP_ONLY));
    }
}

static void matrix_release_all(void)
{
    for (unsigned c = 0; c < 3; c++)
        gpio_set_direction(cols[c], GPIO_MODE_INPUT);
}

static void matrix_select_column(unsigned c)
{
    gpio_set_level(cols[c], 0);
    gpio_set_direction(cols[c], GPIO_MODE_OUTPUT);
}

static uint8_t matrix_read_low_rows(void)
{
    uint8_t low = 0;
    for (unsigned r = 0; r < 3; r++)
        if (!gpio_get_level(rows[r])) low |= 1U << r;
    return low;
}

static uint16_t matrix_scan(void)
{
    const matrix_io_t io = {matrix_release_all, matrix_select_column, matrix_read_low_rows, esp_rom_delay_us};
    matrix_sample_t sample = matrix_scan_cycle(&io);
    matrix_invalid_rows = sample.invalid_rows;
    return sample.keys;
}

static bool enter_sleep(void)
{
    /* Все столбцы LOW + удержание уровней во сне: любая кнопка опускает
     * свою строку в LOW. Подтяжки столбцов отключаем, чтобы не тратить ток. */
    /* Caller holds the Zigbee lock, with no outstanding transmission. */
    for (unsigned c = 0; c < 3; c++) {
        ESP_ERROR_CHECK(gpio_set_level(cols[c], 0));
        ESP_ERROR_CHECK(gpio_set_pull_mode(cols[c], GPIO_FLOATING));
        ESP_ERROR_CHECK(gpio_set_direction(cols[c], GPIO_MODE_OUTPUT));
        ESP_ERROR_CHECK(gpio_hold_en(cols[c]));
    }
    esp_rom_delay_us(10);
    for (unsigned r = 0; r < 3; r++) {
        if (!gpio_get_level(rows[r])) {
            matrix_init();
            return false; /* A press raced the idle check. */
        }
    }
    /* Battery mode: wake only from matrix rows, with no periodic timer. */
    ESP_ERROR_CHECK(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL));
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io((1ULL << GPIO_NUM_0) |
                                                    (1ULL << GPIO_NUM_1) |
                                                    (1ULL << GPIO_NUM_2),
                                                    ESP_EXT1_WAKEUP_ANY_LOW));
    memset(&wake_capture, 0, sizeof(wake_capture));
    esp_set_deep_sleep_wake_stub(micropad_wake_stub);
    ESP_LOGI(TAG, "STATE: ENTERING DEEP SLEEP | queued=%u | wake: KEY ONLY (timer OFF) | previous cause=0x%lx EXT1 rows=0x%llx",
             pending.count, (unsigned long)esp_sleep_get_wakeup_causes(),
             (unsigned long long)esp_sleep_get_ext1_wakeup_status());
    fflush(stdout);
    /* USB disappears in deep sleep. Let the host drain the status line,
     * continuing to check keys so this pause cannot swallow a new press. */
    for (unsigned tick = 0; tick < 50; tick++) {
        vTaskDelay(pdMS_TO_TICKS(5));
        for (unsigned r = 0; r < 3; r++) {
            if (!gpio_get_level(rows[r])) {
                matrix_init();
                ESP_LOGI(TAG, "STATE: AWAKE | sleep cancelled: key pressed");
                return false;
            }
        }
    }
    /* EXT1 uses the separate RTC mux/pulls on C6. Digital pullups alone
     * do not keep the released rows HIGH after that mux switches. */
    for (unsigned r = 0; r < 3; r++) {
        ESP_ERROR_CHECK(rtc_gpio_init(rows[r]));
        ESP_ERROR_CHECK(rtc_gpio_set_direction(rows[r], RTC_GPIO_MODE_INPUT_ONLY));
        ESP_ERROR_CHECK(rtc_gpio_pullup_en(rows[r]));
        ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(rows[r]));
    }
    esp_rom_delay_us(150);
    for (unsigned r = 0; r < 3; r++) {
        if (!rtc_gpio_get_level(rows[r])) {
            matrix_init();
            ESP_LOGI(TAG, "STATE: AWAKE | sleep cancelled: RTC row LOW");
            return false;
        }
    }
    // Гасим LED только после всех проверок отмены сна; HIGH сохраняется в deep sleep.
    ESP_ERROR_CHECK(gpio_set_level(STATUS_LED_GPIO, 1));
    ESP_ERROR_CHECK(gpio_hold_en(STATUS_LED_GPIO));
    esp_deep_sleep_start();
    return true;
}

static void schedule_commissioning(uint32_t mode)
{
    retry_mode = mode;
    retry_at = now_ms();
    retry_pending = true;
}

static bool signal_handler(const ezb_app_signal_t *signal)
{
    ezb_app_signal_type_t type = ezb_app_signal_get_type(signal);
    switch (type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT:
    case EZB_BDB_SIGNAL_STEERING: {
        ezb_bdb_comm_status_t status = *(ezb_bdb_comm_status_t *)ezb_app_signal_get_params(signal);
        if (status == EZB_BDB_STATUS_SUCCESS) {
            if (ezb_bdb_is_factory_new()) {
                xEventGroupSetBits(network, INTERVIEW);
                ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
            } else {
                retry_pending = false;
                xEventGroupSetBits(network, READY);
                ESP_LOGI(TAG, "Zigbee ready: address=0x%04x channel=%u",
                         ezb_nwk_get_short_address(), ezb_nwk_get_current_channel());
            }
        } else {
            xEventGroupClearBits(network, READY);
            ESP_LOGW(TAG, "Commissioning failed: 0x%02x; retrying", status);
            schedule_commissioning(type == EZB_BDB_SIGNAL_STEERING ?
                                  EZB_BDB_MODE_NETWORK_STEERING : EZB_BDB_MODE_INITIALIZATION);
        }
        break;
    }
    case EZB_ZDO_SIGNAL_LEAVE:
        xEventGroupClearBits(network, READY);
        schedule_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
        break;
    default:
        ESP_LOGI(TAG, "Zigbee: %s", ezb_app_signal_to_string(type));
        break;
    }
    return true;
}

static void create_device(void)
{
    /* Здесь задаются модель/производитель, видимые в Zigbee2MQTT.
     * Первый байт строк ниже — их длина: Monco=5, Micropad=8. */
    const ezb_af_ep_config_t ep_cfg = {
        .ep_id = EP, .app_profile_id = EZB_AF_HA_PROFILE_ID, .app_device_id = 0xfff0,
    };
    ezb_af_device_desc_t device = ezb_af_create_device_desc();
    ezb_af_ep_desc_t ep = ezb_af_create_endpoint_desc(&ep_cfg);
    assert(device && ep);
    const ezb_zcl_basic_cluster_server_config_t basic_cfg = {.zcl_version = 8, .power_source = 3};
    ezb_zcl_cluster_desc_t basic = ezb_zcl_basic_create_cluster_desc(&basic_cfg, EZB_ZCL_CLUSTER_SERVER);
    assert(basic);
    ESP_ERROR_CHECK(ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, "\x05" "Monco"));
    ESP_ERROR_CHECK(ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, "\x08" "Micropad"));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep, basic));
    ezb_zcl_cluster_desc_t identify = ezb_zcl_identify_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_SERVER);
    assert(identify);
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep, identify));
    const ezb_zcl_multistate_input_cluster_server_config_t mi_cfg = {.number_of_states = 29};
    ezb_zcl_cluster_desc_t mi = ezb_zcl_multistate_input_create_cluster_desc(&mi_cfg, EZB_ZCL_CLUSTER_SERVER);
    assert(mi);
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep, mi));
    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(device, ep));
    ESP_ERROR_CHECK(ezb_af_device_desc_register(device));
}

static void zigbee_task(void *arg)
{
    /* End Device может спать. keep_alive=1000 работает при запущенном
     * стеке и НЕ будит deep sleep. ed_timeout=2048 мин — запрос родителю
     * хранить запись о нас ~34 ч; это не таймер пробуждения платы.
     * Значение ed_timeout ниже продублировано в ezb_nwk_set_ed_timeout(). */
    esp_zigbee_config_t cfg = {
        .device_config = {
            .device_type = EZB_NWK_DEVICE_TYPE_END_DEVICE,
            .install_code_policy = false,
            .zed_config = {.ed_timeout = EZB_NWK_ED_TIMEOUT_2048MIN, .keep_alive = 1000},
        },
        .platform_config = {
            .storage_partition_name = "nvs",
            .radio_config = {.radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE},
        },
    };
    ESP_ERROR_CHECK(esp_zigbee_init(&cfg));
    /* Request a long parent retention interval; acceptance is parent-dependent. */
    ezb_nwk_set_ed_timeout(EZB_NWK_ED_TIMEOUT_2048MIN);
    create_device();
    ezb_nwk_set_rx_on_when_idle(false);
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(0x07fff800));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(0));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(signal_handler));
    ESP_ERROR_CHECK(esp_zigbee_start(false));
    xEventGroupSetBits(network, INITIALIZED);
    ESP_ERROR_CHECK(esp_zigbee_launch_mainloop());
    vTaskDelete(NULL);
}

static void tx_confirm(ezb_af_user_cnf_t *cnf, void *context)
{
    tx_result_t result = {.token = (uint32_t)(uintptr_t)context, .status = cnf->status};
    xQueueSend(confirmations, &result, 0);
}

static ezb_err_t send_event(uint16_t value, uint32_t token)
{
    /* Отправляем report координатору 0x0000, endpoint 1.
     * Завершение передачи приходит в tx_confirm(); это не подтверждение
     * выполнения автоматизации в Home Assistant. */
    uint16_t zero = 0;
    /* Force a change even for successive identical gestures. */
    ezb_zcl_set_attr_value(EP, CLUSTER, EZB_ZCL_CLUSTER_SERVER, VALUE_ATTR,
                         EZB_ZCL_STD_MANUF_CODE, &zero, false);
    ezb_zcl_status_t status = ezb_zcl_set_attr_value(EP, CLUSTER, EZB_ZCL_CLUSTER_SERVER,
                                                   VALUE_ATTR, EZB_ZCL_STD_MANUF_CODE, &value, false);
    if (status != 0) return EZB_ERR_FAIL;
    ezb_zcl_report_attr_cmd_t cmd = {
        .cmd_ctrl = {
            .dst_addr = {.addr_mode = EZB_ADDR_MODE_SHORT, .u.short_addr = 0x0000},
            .dst_ep = 1, .src_ep = EP, .cluster_id = CLUSTER,
            .manuf_code = EZB_ZCL_STD_MANUF_CODE,
            .fc = {.direction = 1, .dis_default_rsp = 1},
            .cnf_ctx = {.cb = tx_confirm, .user_ctx = (void *)(uintptr_t)token},
        },
        .payload = {.attr_id = VALUE_ATTR},
    };
    return ezb_zcl_report_attr_cmd_req(&cmd);
}

void app_main(void)
{
    matrix_init(); /* Capture before NVS/radio startup. */
    uint16_t first = matrix_scan();
    uint32_t start = now_ms();
    uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    bool key_wake = (wake_causes & BIT(ESP_SLEEP_WAKEUP_EXT1)) != 0;
    uint32_t network_window = key_wake ? RECONNECT_WINDOW_MS : JOIN_WINDOW_MS;
    const char *wake_reason = (wake_causes & BIT(ESP_SLEEP_WAKEUP_EXT1)) ? "KEY" :
                              (wake_causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) ? "TIMER" : "POWER/RESET";
    ESP_LOGI(TAG, "STATE: AWAKE | wake reason: %s | captured events=%u", wake_reason,
             key_wake && wake_capture.valid ? wake_capture.count : 0);
    ESP_LOGI(TAG, "BATTERY MODE: no timer wake; idle=%ums; wait for TX confirmation", (unsigned)IDLE_MS);
    for (unsigned i = 0; i < 9; i++) {
        buttons[i].raw = (first >> i) & 1;
        buttons[i].changed_at = start;
    }
    if (key_wake && wake_capture.valid) {
        wake_gesture_restore(&wake_capture, buttons, start);
        for (unsigned i = 0; i < wake_capture.count; i++) {
            uint16_t value = wake_capture.events[i];
            ESP_LOGI(TAG, "Wake gesture captured => %u", value);
            if (pending.count < QUEUE_SIZE) {
                pending.values[(pending.head + pending.count) % QUEUE_SIZE] = value;
                pending.count++;
            } else ESP_LOGE(TAG, "Event queue full; wake event %u dropped", value);
        }
    }
    memset(&wake_capture, 0, sizeof(wake_capture));
    /* XIAO internal antenna; active-low user LED off. */
    gpio_set_direction(GPIO_NUM_3, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_3, 0);
    gpio_set_direction(GPIO_NUM_14, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_14, 0);
    status_led_init();
    gpio_set_direction(GPIO_NUM_9, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_9, GPIO_PULLUP_ONLY);
    ESP_ERROR_CHECK(nvs_flash_init()); /* Never silently erase pairing data. */
    network = xEventGroupCreate();
    confirmations = xQueueCreate(8, sizeof(tx_result_t));
    assert(network && confirmations);
    assert(xTaskCreate(zigbee_task, "zigbee", 6144, NULL, 5, NULL) == pdPASS);
    ESP_LOGI(TAG, "Rows D0/D1/D2; columns D7/D8/D9; wake=0x%lx; first=0x%03x",
             (unsigned long)esp_sleep_get_wakeup_causes(), first);

    uint32_t activity = now_ms(), tx_at = 0, token = 0, boot_at = 0, status_at = now_ms();
    uint32_t ready_at = start, awake_guard = key_wake ? READY_SETTLE_MS : SETUP_AWAKE_MS;
    unsigned attempts = 0;
    bool in_flight = false, was_ready = false, boot_down = false;
    chord_state_t chord = {0};
    bool factory_requested = false;
    for (;;) {
        uint32_t now = now_ms();
        uint16_t raw = matrix_scan();
        bool was_suppressed = chord.suppress;
        chord_action_t was_armed = chord.armed;
        chord_action_t command = chord_update(&chord, raw, matrix_invalid_rows != 0, now);
        led_chord_candidate = chord.candidate;
        led_chord_armed = chord.armed != CHORD_NONE;
        if (chord.armed != CHORD_NONE && chord.armed != was_armed)
            ESP_LOGW(TAG, "Combination ready: %s; release all keys to confirm",
                     chord.armed == CHORD_FACTORY ? "FACTORY RESET" : "RESTART");
        // Выполняем после отпускания, чтобы удерживаемая комбинация не сбрасывала плату по кругу.
        if (command == CHORD_RESTART) {
            ESP_LOGW(TAG, "Keys 1+3: restart, pairing preserved");
            fflush(stdout);
            esp_restart();
        }
        if (command == CHORD_FACTORY) factory_requested = true;
        bool suppress_keys = chord.suppress || was_suppressed || factory_requested;
        bool busy = raw != 0 || matrix_invalid_rows != 0;
        busy |= suppress_keys;
        for (unsigned i = 0; i < 9; i++) {
            if (suppress_keys) {
                memset(&buttons[i], 0, sizeof(buttons[i]));
                continue;
            }
            if (matrix_invalid_rows & (1U << (i / 3))) {
                /* A bad electrical sample is neither a press nor a release. */
                memset(&buttons[i], 0, sizeof(buttons[i]));
                continue;
            }
            button_action_t action = button_update(&buttons[i], (raw >> i) & 1, now);
            busy |= button_busy(&buttons[i]);
            if (action != BUTTON_NONE) {
                // i=0..8: single -> 1..9, double -> 11..19, hold -> 21..29.
                uint16_t value = i + 1 + (action - 1) * 10;
                ESP_LOGI(TAG, "button_%u_%s => %u", i + 1,
                         action == BUTTON_SINGLE ? "press" : action == BUTTON_DOUBLE ? "double" : "hold", value);
                if (pending.count < QUEUE_SIZE) {
                    pending.values[(pending.head + pending.count) % QUEUE_SIZE] = value;
                    pending.count++;
                } else ESP_LOGE(TAG, "Event queue full; event %u dropped", value);
                activity = now;
            }
        }
        // Ожидание double, нажатая клавиша и ошибка матрицы тоже считаются активностью.
        if (busy) activity = now;
        bool boot = !gpio_get_level(GPIO_NUM_9);
        if (boot && !boot_down) boot_at = now;
        boot_down = boot;
        if (boot) activity = now;
        EventBits_t bits = xEventGroupGetBits(network);
        bool ready = bits & READY;
        if (ready && !was_ready) {
            ready_at = now;
            awake_guard = (!key_wake || (bits & INTERVIEW)) ? SETUP_AWAKE_MS : READY_SETTLE_MS;
        }
        was_ready = ready;
        if (now - status_at >= 5000) {
            status_at = now;
            if (matrix_invalid_rows) {
                ESP_LOGE(TAG, "MATRIX: LOW with all columns released; rows=0x%x (bit0=D0 bit1=D1 bit2=D2); events suppressed",
                         matrix_invalid_rows);
            }
            ESP_LOGI(TAG, "STATE: AWAKE | Zigbee=%s | idle=%lums/%ums | keys=0x%03x | queued=%u | TX=%s",
                     ready ? "CONNECTED" : "SEARCHING", (unsigned long)(now - activity), (unsigned)IDLE_MS,
                     raw, pending.count, in_flight ? "WAIT_CONFIRM" : "IDLE");
            if (!ready) {
                uint32_t left = now - start < network_window ? network_window - (now - start) : 0;
                ESP_LOGI(TAG, "No network: sleep allowed after %lu s (and released keys)",
                         (unsigned long)((left + 999) / 1000));
            }
        }
        if ((bits & INITIALIZED) && esp_zigbee_lock_acquire(pdMS_TO_TICKS(2))) {
            // BOOT держать 5 с: сброс сопряжения. Для доступа к окну настройки нажать RESET.
            if (factory_requested || (boot && now - boot_at >= 5000)) {
                ESP_LOGW(TAG, "Factory reset: erase Zigbee pairing and queued events, restart");
                memset(&pending, 0, sizeof(pending));
                esp_zigbee_factory_reset();
            }
            if (retry_pending && now - retry_at >= RETRY_MS) {
                retry_pending = false;
                if (ezb_bdb_start_top_level_commissioning(retry_mode) != EZB_ERR_NONE)
                    schedule_commissioning(retry_mode);
            }
            tx_result_t result;
            while (xQueueReceive(confirmations, &result, 0) == pdTRUE) {
                if (!in_flight || result.token != token) continue;
                in_flight = false;
                tx_at = now;
                if (result.status == 0) {
                    led_has_confirmation = true;
                    led_confirmed_at = now;
                    status_led_update(now);
                    ESP_LOGI(TAG, "TX confirmed: %u", pending.values[pending.head]);
                    pending.head = (pending.head + 1) % QUEUE_SIZE;
                    pending.count--;
                    attempts = 0;
                    uint16_t zero = 0;
                    ezb_zcl_set_attr_value(EP, CLUSTER, EZB_ZCL_CLUSTER_SERVER, VALUE_ATTR,
                                         EZB_ZCL_STD_MANUF_CODE, &zero, false);
                } else ESP_LOGW(TAG, "TX failed: 0x%02x", result.status);
            }
            if (in_flight && now - tx_at >= TX_TIMEOUT_MS) {
                ESP_LOGW(TAG, "TX confirmation timeout; retained for next wake");
                in_flight = false;
                attempts = 3;
            }
            if (ready && pending.count && !in_flight && attempts < 3 &&
                (attempts == 0 || now - tx_at >= RETRY_MS)) {
                attempts++;
                token++;
                tx_at = now;
                ezb_err_t err = send_event(pending.values[pending.head], token);
                in_flight = err == EZB_ERR_NONE;
                if (!in_flight) ESP_LOGW(TAG, "Report request failed: %d", err);
            }
            /* Главные условия сна (проверяются вместе):
             * 1) Нет незавершённого жеста, ошибки матрицы, BOOT и ожидаемого TX;
             *    прошло IDLE_MS без активности.
             * 2) Сеть готова, очередь пуста (или 3 попытки исчерпаны), истекли
             *    окно 1/20 с после READY и пауза 250 мс после TX;
             *    ИЛИ сеть недоступна и закончились 25/120 с на подключение.
             * После этого enter_sleep() ещё даёт USB 250 мс и проверяет кнопки.
             * Эти интервалы частично идут одновременно — их нельзя просто сложить. */
            bool can_sleep = !busy && !boot && !in_flight && now - activity >= IDLE_MS;
            bool drained = ready && (!pending.count || attempts >= 3) &&
                           now - ready_at >= awake_guard && now - tx_at >= TX_SETTLE_MS;
            bool offline_timeout = !ready && now - start >= network_window;
            if (can_sleep && (drained || offline_timeout)) enter_sleep();
            esp_zigbee_lock_release();
        }
        status_led_update(now);
        vTaskDelay(pdMS_TO_TICKS(5)); // Пауза между проходами; само сканирование тоже занимает время.
    }
}
