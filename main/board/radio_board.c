/*
 * SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hardware setup is adapted from the Waveshare ESP32-S3-Touch-LCD-1.54
 * ESP-IDF LVGL example. Board pin assignments are kept here so application
 * code does not depend on GPIO details.
 */
#include <stdint.h>
#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lvgl_port.h"
#include "esp_lcd_touch_cst816s.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "radio_board.h"

#define LCD_H_RES              240
#define LCD_V_RES              240
#define LCD_DRAW_BUFFER_LINES  50
#define LCD_SPI_HOST           SPI3_HOST
#define LCD_PIXEL_CLOCK_HZ     (40 * 1000 * 1000)

#define LCD_GPIO_SCLK          GPIO_NUM_38
#define LCD_GPIO_MOSI          GPIO_NUM_39
#define LCD_GPIO_RST           GPIO_NUM_40
#define LCD_GPIO_DC            GPIO_NUM_45
#define LCD_GPIO_CS            GPIO_NUM_21
#define LCD_GPIO_BACKLIGHT     GPIO_NUM_46

#define TOUCH_I2C_PORT         I2C_NUM_0
#define TOUCH_GPIO_SCL         GPIO_NUM_41
#define TOUCH_GPIO_SDA         GPIO_NUM_42
#define TOUCH_GPIO_INT         GPIO_NUM_48
#define TOUCH_GPIO_RST         GPIO_NUM_47

#define AUDIO_I2S_PORT         I2S_NUM_0
#define AUDIO_GPIO_MCLK        GPIO_NUM_8
#define AUDIO_GPIO_BCLK        GPIO_NUM_9
#define AUDIO_GPIO_LRCK        GPIO_NUM_10
#define AUDIO_GPIO_DOUT        GPIO_NUM_12
#define AUDIO_GPIO_PA          GPIO_NUM_7
#define AUDIO_I2S_MCLK_MULTIPLE I2S_MCLK_MULTIPLE_256
#define BATTERY_POWER_HOLD_GPIO GPIO_NUM_2
#define BATTERY_ADC_GPIO        GPIO_NUM_1
#define BATTERY_CHARGE_STATUS_GPIO GPIO_NUM_3
#define BATTERY_DIVIDER_RATIO   3
#define BATTERY_ADC_SAMPLES     12

static const char *TAG = "radio_board";
static esp_lcd_panel_io_handle_t s_lcd_io;
static esp_lcd_panel_handle_t s_lcd_panel;
static esp_lcd_touch_handle_t s_touch;
static i2c_master_bus_handle_t s_i2c_bus;
static i2s_chan_handle_t s_audio_tx;
static esp_codec_dev_handle_t s_audio_codec;
static SemaphoreHandle_t s_audio_lock;
static bool s_audio_initialized;
static bool s_audio_open;
static int s_audio_volume = 55;
static esp_err_t s_touch_init_result = ESP_ERR_INVALID_STATE;
static bool s_backlight_pwm_ready;
static adc_oneshot_unit_handle_t s_battery_adc;
static adc_cali_handle_t s_battery_adc_calibration;
static adc_unit_t s_battery_adc_unit;
static adc_channel_t s_battery_adc_channel;

esp_err_t radio_board_display_set_brightness(uint8_t percent)
{
    if (!s_backlight_pwm_ready) return ESP_ERR_INVALID_STATE;
    const uint8_t bounded = percent > 100 ? 100 : percent;
    const uint32_t duty = ((uint32_t)bounded * 1023U) / 100U;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty),
                        TAG, "could not set LCD brightness");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void audio_mute_unlocked(void)
{
    gpio_set_level(AUDIO_GPIO_PA, 0);
}

esp_err_t radio_board_power_hold_init(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << BATTERY_POWER_HOLD_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "power hold GPIO init failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(BATTERY_POWER_HOLD_GPIO, 1), TAG,
                        "could not latch battery power");
    /* Keep the speaker amplifier disabled before the codec is initialized. */
    const gpio_config_t amplifier = {
        .pin_bit_mask = 1ULL << AUDIO_GPIO_PA,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&amplifier), TAG, "amplifier GPIO init failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(AUDIO_GPIO_PA, 0), TAG,
                        "could not disable speaker amplifier");
    ESP_LOGI(TAG, "Battery power latch enabled");
    return ESP_OK;
}

void radio_board_power_off(void)
{
    ESP_LOGI(TAG, "Apagado solicitado");
    gpio_set_level(AUDIO_GPIO_PA, 0);
    radio_board_display_set_brightness(0);
    gpio_set_level(BATTERY_POWER_HOLD_GPIO, 0);
    gpio_hold_en(BATTERY_POWER_HOLD_GPIO);
    gpio_deep_sleep_hold_en();
    esp_deep_sleep_start();
}

static esp_err_t display_init(void)
{
    const ledc_timer_config_t backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&backlight_timer), TAG,
                        "backlight PWM timer init failed");
    const ledc_channel_config_t backlight_channel = {
        .gpio_num = LCD_GPIO_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&backlight_channel), TAG,
                        "backlight PWM channel init failed");
    s_backlight_pwm_ready = true;

    const spi_bus_config_t spi_config = {
        .sclk_io_num = LCD_GPIO_SCLK,
        .mosi_io_num = LCD_GPIO_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_H_RES * LCD_DRAW_BUFFER_LINES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &spi_config, SPI_DMA_CH_AUTO),
                        TAG, "LCD SPI init failed");

    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_GPIO_DC,
        .cs_gpio_num = LCD_GPIO_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                                  &io_config, &s_lcd_io),
                        TAG, "LCD panel IO init failed");

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_GPIO_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_lcd_io, &panel_config, &s_lcd_panel),
                        TAG, "ST7789 init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_lcd_panel), TAG, "LCD reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_lcd_panel), TAG, "LCD init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_lcd_panel, true), TAG,
                        "LCD color setup failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_lcd_panel, true), TAG,
                        "LCD enable failed");
    return radio_board_display_set_brightness(35);
}

static esp_err_t touch_init(void)
{
    const i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = TOUCH_I2C_PORT,
        .scl_io_num = TOUCH_GPIO_SCL,
        .sda_io_num = TOUCH_GPIO_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &s_i2c_bus), TAG,
                        "touch I2C bus init failed");

    esp_lcd_panel_io_handle_t touch_io = NULL;
    const esp_lcd_panel_io_i2c_config_t touch_io_config = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 8,
        .flags.disable_control_phase = true,
        .scl_speed_hz = 400 * 1000,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus, &touch_io_config, &touch_io),
                        TAG, "touch panel IO init failed");

    const esp_lcd_touch_config_t touch_config = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = TOUCH_GPIO_RST,
        .int_gpio_num = TOUCH_GPIO_INT,
        .flags = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
    };
    return esp_lcd_touch_new_i2c_cst816s(touch_io, &touch_config, &s_touch);
}

static esp_err_t lvgl_init(void)
{
    const lvgl_port_cfg_t lvgl_config = {
        .task_priority = 4,
        .task_stack = 1024 * 10,
        .task_affinity = -1,
        .task_max_sleep_ms = 500,
        .timer_period_ms = 10,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_config), TAG, "LVGL init failed");

    const lvgl_port_display_cfg_t display_config = {
        .io_handle = s_lcd_io,
        .panel_handle = s_lcd_panel,
        .buffer_size = LCD_H_RES * LCD_DRAW_BUFFER_LINES,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,
        },
    };
    lv_display_t *display = lvgl_port_add_disp(&display_config);
    ESP_RETURN_ON_FALSE(display != NULL, ESP_FAIL, TAG, "LVGL display registration failed");

    if (s_touch_init_result == ESP_OK && s_touch != NULL) {
        const lvgl_port_touch_cfg_t touch_config = {
            .disp = display,
            .handle = s_touch,
        };
        if (lvgl_port_add_touch(&touch_config) == NULL) {
            s_touch_init_result = ESP_FAIL;
            s_touch = NULL;
            ESP_LOGW(TAG, "Touch registration failed; display remains available");
        }
    }
    return ESP_OK;
}

static esp_err_t battery_monitor_init(void)
{
    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(BATTERY_ADC_GPIO,
                                                  &s_battery_adc_unit,
                                                  &s_battery_adc_channel),
                        TAG, "battery ADC pin mapping failed");

    const adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = s_battery_adc_unit,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_config, &s_battery_adc),
                        TAG, "battery ADC init failed");

    const adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_battery_adc,
                                                   s_battery_adc_channel,
                                                   &channel_config),
                        TAG, "battery ADC channel setup failed");

    /* Waveshare routes ETA6098 STAT to GPIO3 (CHG_STAT). STAT is open-drain
       and active-low while charging; use a pull-up for the completed state. */
    const gpio_config_t charge_status_config = {
        .pin_bit_mask = 1ULL << BATTERY_CHARGE_STATUS_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&charge_status_config), TAG,
                        "battery charge status GPIO init failed");

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    const adc_cali_curve_fitting_config_t calibration_config = {
        .unit_id = s_battery_adc_unit,
        .chan = s_battery_adc_channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    const esp_err_t calibration_err = adc_cali_create_scheme_curve_fitting(
        &calibration_config, &s_battery_adc_calibration);
    if (calibration_err != ESP_OK) {
        s_battery_adc_calibration = NULL;
        ESP_LOGW(TAG, "Battery ADC calibration unavailable; using approximate conversion");
    }
#else
    ESP_LOGW(TAG, "Battery ADC calibration scheme unavailable; using approximate conversion");
#endif

    return ESP_OK;
}

static uint8_t battery_percent_from_voltage(uint32_t millivolts)
{
    static const struct {
        uint16_t millivolts;
        uint8_t percent;
    } curve[] = {
        {3200, 0}, {3400, 5}, {3500, 10}, {3600, 20}, {3700, 40},
        {3800, 60}, {3900, 75}, {4000, 85}, {4100, 95}, {4200, 100},
    };
    if (millivolts <= curve[0].millivolts) return curve[0].percent;
    const size_t last = sizeof(curve) / sizeof(curve[0]) - 1;
    if (millivolts >= curve[last].millivolts) return curve[last].percent;
    for (size_t i = 1; i <= last; ++i) {
        if (millivolts <= curve[i].millivolts) {
            const uint32_t span_mv = curve[i].millivolts - curve[i - 1].millivolts;
            const uint32_t offset_mv = millivolts - curve[i - 1].millivolts;
            const uint32_t span_pct = curve[i].percent - curve[i - 1].percent;
            return (uint8_t)(curve[i - 1].percent + (offset_mv * span_pct) / span_mv);
        }
    }
    return 100;
}

static esp_err_t battery_read_voltage(uint16_t *millivolts)
{
    if (millivolts == NULL) return ESP_ERR_INVALID_ARG;
    if (s_battery_adc == NULL) return ESP_ERR_INVALID_STATE;

    uint32_t sum_mv = 0;
    for (unsigned int i = 0; i < BATTERY_ADC_SAMPLES; ++i) {
        int adc_mv = 0;
        esp_err_t err;
        if (s_battery_adc_calibration != NULL) {
            err = adc_oneshot_get_calibrated_result(s_battery_adc,
                                                    s_battery_adc_calibration,
                                                    s_battery_adc_channel, &adc_mv);
        } else {
            int raw = 0;
            err = adc_oneshot_read(s_battery_adc, s_battery_adc_channel, &raw);
            if (err == ESP_OK) adc_mv = (raw * 3100) / 4095;
        }
        if (err != ESP_OK) return err;
        sum_mv += (uint32_t)adc_mv;
    }

    const uint32_t battery_mv =
        (sum_mv * BATTERY_DIVIDER_RATIO + BATTERY_ADC_SAMPLES / 2) /
        BATTERY_ADC_SAMPLES;
    if (battery_mv < 2500 || battery_mv > 4500) return ESP_ERR_INVALID_RESPONSE;
    *millivolts = (uint16_t)battery_mv;
    return ESP_OK;
}

esp_err_t radio_board_battery_read(uint16_t *millivolts, uint8_t *percent)
{
    if (millivolts == NULL || percent == NULL) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(battery_read_voltage(millivolts), TAG,
                        "battery voltage read failed");
    *percent = battery_percent_from_voltage(*millivolts);
    return ESP_OK;
}

esp_err_t radio_board_battery_read_percent(uint8_t *percent)
{
    if (percent == NULL) return ESP_ERR_INVALID_ARG;
    uint16_t millivolts = 0;
    return radio_board_battery_read(&millivolts, percent);
}

esp_err_t radio_board_battery_is_charging(bool *charging)
{
    if (charging == NULL) return ESP_ERR_INVALID_ARG;
    const int level = gpio_get_level(BATTERY_CHARGE_STATUS_GPIO);
    if (level < 0) return ESP_FAIL;
    *charging = (level == 0);
    return ESP_OK;
}

esp_err_t radio_board_init(void)
{
    const esp_err_t battery_err = battery_monitor_init();
    if (battery_err != ESP_OK) {
        ESP_LOGW(TAG, "Battery indicator unavailable: %s", esp_err_to_name(battery_err));
    }
    ESP_RETURN_ON_ERROR(display_init(), TAG, "display init failed");
    s_touch_init_result = touch_init();
    if (s_touch_init_result != ESP_OK) {
        s_touch = NULL;
        ESP_LOGW(TAG, "Touch unavailable; continuing with display only: %s",
                 esp_err_to_name(s_touch_init_result));
    }
    return lvgl_init();
}

esp_err_t radio_board_touch_status(void)
{
    return s_touch_init_result;
}

esp_err_t radio_board_audio_init(void)
{
    if (s_audio_initialized) {
        return ESP_OK;
    }
    if (s_audio_tx != NULL) {
        ESP_LOGE(TAG, "audio I2S channel remains after failed cleanup");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_audio_lock == NULL) {
        s_audio_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_audio_lock != NULL, ESP_ERR_NO_MEM, TAG,
                            "audio mutex allocation failed");
    }

    const audio_codec_data_if_t *data_if = NULL;
    const audio_codec_ctrl_if_t *control_if = NULL;
    const audio_codec_gpio_if_t *gpio_if = NULL;
    const audio_codec_if_t *codec_if = NULL;
    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    /* Clear each DMA block after consumption so starvation cannot loop stale PCM. */
    channel_config.auto_clear_after_cb = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, &s_audio_tx, NULL), TAG,
                        "audio I2S channel init failed");

    const i2s_std_config_t std_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AUDIO_GPIO_MCLK,
            .bclk = AUDIO_GPIO_BCLK,
            .ws = AUDIO_GPIO_LRCK,
            .dout = AUDIO_GPIO_DOUT,
            .din = GPIO_NUM_NC,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    esp_err_t err = i2s_channel_init_std_mode(s_audio_tx, &std_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio I2S standard mode init failed: %s", esp_err_to_name(err));
        goto fail;
    }

    audio_codec_i2s_cfg_t i2s_codec_config = {
        .port = AUDIO_I2S_PORT,
        .rx_handle = NULL,
        .tx_handle = s_audio_tx,
    };
    data_if = audio_codec_new_i2s_data(&i2s_codec_config);
    if (data_if == NULL) {
        ESP_LOGE(TAG, "audio I2S data interface failed");
        err = ESP_FAIL;
        goto fail;
    }

    audio_codec_i2c_cfg_t i2c_codec_config = {
        .port = TOUCH_I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c_bus,
    };
    control_if = audio_codec_new_i2c_ctrl(&i2c_codec_config);
    if (control_if == NULL) {
        ESP_LOGE(TAG, "audio I2C control interface failed");
        err = ESP_FAIL;
        goto fail;
    }

    gpio_if = audio_codec_new_gpio();
    if (gpio_if == NULL) {
        ESP_LOGE(TAG, "audio GPIO interface failed");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    es8311_codec_cfg_t es8311_config = {
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .ctrl_if = control_if,
        .gpio_if = gpio_if,
        .pa_pin = AUDIO_GPIO_PA,
        .pa_reverted = false,
        .use_mclk = true,
        .mclk_div = AUDIO_I2S_MCLK_MULTIPLE,
        .hw_gain = {
            .pa_voltage = 5.0f,
            .codec_dac_voltage = 3.3f,
        },
    };
    codec_if = es8311_codec_new(&es8311_config);
    if (codec_if == NULL) {
        ESP_LOGE(TAG, "ES8311 codec init failed");
        err = ESP_FAIL;
        goto fail;
    }

    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_audio_codec = esp_codec_dev_new(&device_config);
    if (s_audio_codec == NULL) {
        ESP_LOGE(TAG, "audio codec device init failed");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    s_audio_initialized = true;
    ESP_LOGI(TAG, "ES8311 audio output ready");
    return ESP_OK;

fail:
    if (codec_if != NULL && audio_codec_delete_codec_if(codec_if) != 0)
        ESP_LOGW(TAG, "audio codec interface cleanup failed");
    if (gpio_if != NULL && audio_codec_delete_gpio_if(gpio_if) != 0)
        ESP_LOGW(TAG, "audio GPIO interface cleanup failed");
    if (control_if != NULL && audio_codec_delete_ctrl_if(control_if) != 0)
        ESP_LOGW(TAG, "audio control interface cleanup failed");
    if (data_if != NULL && audio_codec_delete_data_if(data_if) != 0)
        ESP_LOGW(TAG, "audio data interface cleanup failed");
    const esp_err_t cleanup_err = i2s_del_channel(s_audio_tx);
    if (cleanup_err == ESP_OK) {
        s_audio_tx = NULL;
    } else {
        ESP_LOGW(TAG, "audio I2S channel cleanup failed: %s",
                 esp_err_to_name(cleanup_err));
    }
    return err;
}

esp_err_t radio_board_audio_open(uint32_t sample_rate, uint8_t channels,
                                 uint8_t bits_per_sample)
{
    if (!s_audio_initialized || s_audio_codec == NULL || channels == 0 ||
        sample_rate == 0 || bits_per_sample != 16) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_audio_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_audio_lock, portMAX_DELAY);
    /* Keep the speaker muted through codec reconfiguration and I2S priming. */
    audio_mute_unlocked();
    if (s_audio_open) {
        const esp_err_t close_err = esp_codec_dev_close(s_audio_codec);
        if (close_err != ESP_OK) {
            xSemaphoreGive(s_audio_lock);
            return close_err;
        }
        s_audio_open = false;
    }

    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = sample_rate,
        .channel = channels,
        .channel_mask = channels == 1 ? 0x01 : 0x03,
        .bits_per_sample = bits_per_sample,
        .mclk_multiple = AUDIO_I2S_MCLK_MULTIPLE,
    };
    const esp_err_t open_result = esp_codec_dev_open(s_audio_codec, &sample_info);
    if (open_result != ESP_OK) {
        esp_codec_dev_close(s_audio_codec);
        audio_mute_unlocked();
        xSemaphoreGive(s_audio_lock);
        return open_result;
    }
    s_audio_open = true;
    const esp_err_t volume_result = esp_codec_dev_set_out_vol(s_audio_codec,
                                                               s_audio_volume);
    if (volume_result != ESP_OK) {
        s_audio_open = false;
        esp_codec_dev_close(s_audio_codec);
        audio_mute_unlocked();
    }
    xSemaphoreGive(s_audio_lock);
    return volume_result;
}

esp_err_t radio_board_audio_write(const uint8_t *data, size_t length)
{
    if (data == NULL || length == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_audio_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_audio_lock, portMAX_DELAY);
    if (!s_audio_open) {
        xSemaphoreGive(s_audio_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = esp_codec_dev_write(s_audio_codec, (void *)data, length);
    xSemaphoreGive(s_audio_lock);
    return err;
}

esp_err_t radio_board_audio_set_volume(int volume)
{
    if (volume < 0 || volume > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_audio_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_audio_lock, portMAX_DELAY);
    s_audio_volume = volume;
    const esp_err_t err = s_audio_open
                              ? esp_codec_dev_set_out_vol(s_audio_codec, volume)
                              : ESP_OK;
    xSemaphoreGive(s_audio_lock);
    return err;
}

void radio_board_audio_mute(void)
{
    /* PA is a separate GPIO: mute immediately without waiting for an I2S write. */
    audio_mute_unlocked();
}

void radio_board_audio_unmute(void)
{
    gpio_set_level(AUDIO_GPIO_PA, 1);
}

esp_err_t radio_board_audio_close(void)
{
    if (s_audio_lock == NULL) {
        audio_mute_unlocked();
        return ESP_OK;
    }
    xSemaphoreTake(s_audio_lock, portMAX_DELAY);
    if (!s_audio_open) {
        audio_mute_unlocked();
        xSemaphoreGive(s_audio_lock);
        return ESP_OK;
    }
    s_audio_open = false;
    const esp_err_t err = esp_codec_dev_close(s_audio_codec);
    audio_mute_unlocked();
    xSemaphoreGive(s_audio_lock);
    return err;
}
