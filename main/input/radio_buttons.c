#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "radio_buttons.h"
#include "radio_player.h"
#include "radio_ui.h"

#define BUTTON_PLUS_GPIO GPIO_NUM_4
#define BUTTON_MINUS_GPIO GPIO_NUM_0
#define BUTTON_CENTER_GPIO GPIO_NUM_5
#define BUTTON_SCAN_MS 25
#define BUTTON_DEBOUNCE_SCANS 2
#define BUTTON_POWER_OFF_SCANS 120
#define BUTTON_VOLUME_REPEAT_START_SCANS 20
#define BUTTON_VOLUME_REPEAT_EVERY_SCANS 6
#define BUTTON_VOLUME_STEP 5

static const char *TAG = "radio_buttons";

typedef enum {
    BUTTON_ACTION_VOLUME_UP,
    BUTTON_ACTION_VOLUME_DOWN,
    BUTTON_ACTION_PLAY_OR_POWER_OFF,
} button_action_t;

typedef struct {
    gpio_num_t pin;
    button_action_t action;
    int last_level;
    unsigned int stable_scans;
    unsigned int pressed_scans;
    bool pressed;
    bool long_action_fired;
} button_state_t;

static void volume_step(int delta)
{
    radio_player_volume_step(delta);
    radio_ui_refresh_volume();
}

static void handle_volume_hold(button_state_t *button)
{
    const bool volume_button = button->action == BUTTON_ACTION_VOLUME_UP ||
                               button->action == BUTTON_ACTION_VOLUME_DOWN;
    if (!volume_button ||
        button->pressed_scans < BUTTON_VOLUME_REPEAT_START_SCANS ||
        (button->pressed_scans - BUTTON_VOLUME_REPEAT_START_SCANS) %
                BUTTON_VOLUME_REPEAT_EVERY_SCANS != 0) {
        return;
    }
    volume_step(button->action == BUTTON_ACTION_VOLUME_UP
                    ? BUTTON_VOLUME_STEP : -BUTTON_VOLUME_STEP);
}

static bool button_level_is_stable(button_state_t *button, int level)
{
    if (level == button->last_level) {
        if (button->stable_scans < BUTTON_DEBOUNCE_SCANS) {
            ++button->stable_scans;
        }
    } else {
        button->last_level = level;
        button->stable_scans = 0;
    }
    return button->stable_scans >= BUTTON_DEBOUNCE_SCANS;
}

static void button_pressed(button_state_t *button)
{
    button->pressed = true;
    button->pressed_scans = 0;
    button->long_action_fired = false;
    if (button->action == BUTTON_ACTION_VOLUME_UP) volume_step(BUTTON_VOLUME_STEP);
    if (button->action == BUTTON_ACTION_VOLUME_DOWN) volume_step(-BUTTON_VOLUME_STEP);
}

static void button_held(button_state_t *button)
{
    if (button->pressed_scans < UINT32_MAX) ++button->pressed_scans;
    handle_volume_hold(button);
    if (button->action == BUTTON_ACTION_PLAY_OR_POWER_OFF &&
        !button->long_action_fired &&
        button->pressed_scans >= BUTTON_POWER_OFF_SCANS) {
        button->long_action_fired = true;
        ESP_LOGI(TAG, "Apagado solicitado desde el boton central");
        radio_ui_request_power_off();
    }
}

static void button_released(button_state_t *button)
{
    button->pressed = false;
    if (button->action == BUTTON_ACTION_PLAY_OR_POWER_OFF &&
        !button->long_action_fired) {
        radio_ui_wake_display();
        radio_player_pause_toggle();
    }
}

static void button_poll(button_state_t *button)
{
    const int level = gpio_get_level(button->pin);
    if (!button_level_is_stable(button, level)) return;

    /* The board uses active-low inputs. Dispatch only after debouncing. */
    const bool pressed = level == 0;
    if (pressed && !button->pressed) {
        button_pressed(button);
    } else if (pressed) {
        button_held(button);
    } else if (button->pressed) {
        button_released(button);
    }
}

static void button_task(void *arg)
{
    (void)arg;
    button_state_t buttons[] = {
        {.pin = BUTTON_PLUS_GPIO, .action = BUTTON_ACTION_VOLUME_UP, .last_level = 1},
        {.pin = BUTTON_MINUS_GPIO, .action = BUTTON_ACTION_VOLUME_DOWN, .last_level = 1},
        {.pin = BUTTON_CENTER_GPIO, .action = BUTTON_ACTION_PLAY_OR_POWER_OFF,
         .last_level = 1},
    };

    for (;;) {
        for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) {
            button_poll(&buttons[i]);
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_SCAN_MS));
    }
}

esp_err_t radio_buttons_init(void)
{
    const gpio_config_t button_config = {
        .pin_bit_mask = (1ULL << BUTTON_PLUS_GPIO) |
                        (1ULL << BUTTON_MINUS_GPIO) |
                        (1ULL << BUTTON_CENTER_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&button_config), TAG, "button GPIO setup failed");
    if (xTaskCreate(button_task, "radio_buttons", 4096, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "PLUS GPIO4=subir, BOOT GPIO0=bajar, centro GPIO5=play/pausa; 3 s=apagado");
    return ESP_OK;
}
