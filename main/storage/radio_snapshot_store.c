#include "radio_snapshot_store.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "radio_test_adapters.h"

#define STORE_MAX_RECORD_SIZE \
    (RADIO_SNAPSHOT_HEADER_SIZE + RADIO_SNAPSHOT_STORE_MAX_PAYLOAD_SIZE)

static SemaphoreHandle_t s_store_lock;
static bool s_partition_initialized;
static uint8_t s_slot_a[STORE_MAX_RECORD_SIZE];
static uint8_t s_slot_b[STORE_MAX_RECORD_SIZE];

typedef struct {
    bool present;
    size_t size;
} slot_info_t;

static esp_err_t lock_store(void)
{
    if (s_store_lock == NULL) {
        s_store_lock = xSemaphoreCreateMutex();
        if (s_store_lock == NULL) return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_store_lock, portMAX_DELAY) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    return ESP_OK;
}

static void unlock_store(void)
{
    xSemaphoreGive(s_store_lock);
}

static bool unit_is_valid(const char *unit)
{
    if (unit == NULL) return false;
    const size_t length = strnlen(unit, 14U);
    if (length == 0U || length > 13U) return false;
    for (size_t i = 0; i < length; ++i) {
        const char ch = unit[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
              ch == '_')) {
            return false;
        }
    }
    return true;
}

static esp_err_t make_slot_keys(const char *unit, char key_a[16],
                                char key_b[16])
{
    if (!unit_is_valid(unit)) return ESP_ERR_INVALID_ARG;
    const int a = snprintf(key_a, 16U, "%s_a", unit);
    const int b = snprintf(key_b, 16U, "%s_b", unit);
    if (a < 0 || a > 15 || b < 0 || b > 15) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static esp_err_t open_state(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    if (!s_partition_initialized) return ESP_ERR_INVALID_STATE;
    return nvs_open_from_partition(RADIO_SNAPSHOT_STORE_PARTITION,
                                   RADIO_SNAPSHOT_STORE_NAMESPACE, mode,
                                   handle);
}

static esp_err_t read_slot(nvs_handle_t handle, const char *key,
                           uint8_t *record, slot_info_t *info)
{
    memset(record, 0, STORE_MAX_RECORD_SIZE);
    info->present = false;
    info->size = 0U;

    size_t size = 0U;
    esp_err_t err = nvs_get_blob(handle, key, NULL, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (size > STORE_MAX_RECORD_SIZE) return ESP_ERR_NOT_SUPPORTED;

    size_t actual_size = size;
    err = nvs_get_blob(handle, key, record, &actual_size);
    if (err != ESP_OK) return err;
    if (actual_size != size) return ESP_ERR_INVALID_STATE;
    info->present = true;
    info->size = actual_size;
    return ESP_OK;
}

static esp_err_t select_slots(
    const slot_info_t *a, const slot_info_t *b, uint32_t kind,
    unsigned *selected, radio_snapshot_view_t *view)
{
    if (!a->present && !b->present) return ESP_ERR_NVS_NOT_FOUND;
    const radio_snapshot_result_t result = radio_snapshot_select(
        s_slot_a, a->size, s_slot_b, b->size, kind, selected);
    if (result == RADIO_SNAPSHOT_UNSUPPORTED) return ESP_ERR_NOT_SUPPORTED;
    if (result == RADIO_SNAPSHOT_NO_VALID_SLOT ||
        result == RADIO_SNAPSHOT_AMBIGUOUS) return ESP_ERR_INVALID_STATE;
    if (result != RADIO_SNAPSHOT_OK) return ESP_ERR_INVALID_STATE;

    const uint8_t *record = *selected == 0U ? s_slot_a : s_slot_b;
    const size_t size = *selected == 0U ? a->size : b->size;
    if (radio_snapshot_decode(record, size, kind, view) != RADIO_SNAPSHOT_OK)
        return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

static esp_err_t read_selected(nvs_handle_t handle, const char *key_a,
                               const char *key_b, uint32_t kind,
                               unsigned *selected,
                               radio_snapshot_view_t *view)
{
    slot_info_t a, b;
    esp_err_t err = read_slot(handle, key_a, s_slot_a, &a);
    if (err == ESP_OK) err = read_slot(handle, key_b, s_slot_b, &b);
    if (err != ESP_OK) return err;
    return select_slots(&a, &b, kind, selected, view);
}

static esp_err_t reopen_partition(void)
{
    esp_err_t err = nvs_flash_deinit_partition(RADIO_SNAPSHOT_STORE_PARTITION);
    if (err != ESP_OK) {
        s_partition_initialized = false;
        return err;
    }
    s_partition_initialized = false;
    err = nvs_flash_init_partition(RADIO_SNAPSHOT_STORE_PARTITION);
    if (err == ESP_OK) s_partition_initialized = true;
    return err;
}

static uint32_t next_generation(uint32_t current)
{
    ++current;
    return current == 0U ? 1U : current;
}

static bool selected_matches(const radio_snapshot_view_t *view,
                             uint32_t generation, const uint8_t *payload,
                             size_t payload_size)
{
    return view->generation == generation &&
           view->payload_size == payload_size &&
           (payload_size == 0U ||
            memcmp(view->payload, payload, payload_size) == 0);
}

esp_err_t radio_snapshot_store_init(void)
{
    esp_err_t err = lock_store();
    if (err != ESP_OK) return err;
    if (!s_partition_initialized) {
        err = nvs_flash_init_partition(RADIO_SNAPSHOT_STORE_PARTITION);
        if (err == ESP_OK) s_partition_initialized = true;
    }
    unlock_store();
    return err;
}

esp_err_t radio_snapshot_store_read(
    const char *unit, uint32_t kind, uint8_t *payload, size_t capacity,
    size_t *payload_size, uint32_t *generation)
{
    if (payload_size != NULL) *payload_size = 0U;
    if (generation != NULL) *generation = 0U;
    if (!unit_is_valid(unit) || kind == 0U || payload_size == NULL ||
        generation == NULL || (payload == NULL && capacity != 0U))
        return ESP_ERR_INVALID_ARG;

    char key_a[16], key_b[16];
    esp_err_t err = make_slot_keys(unit, key_a, key_b);
    if (err != ESP_OK) return err;
    err = lock_store();
    if (err != ESP_OK) return err;

    nvs_handle_t handle = 0;
    err = open_state(NVS_READONLY, &handle);
    if (err == ESP_OK) {
        unsigned selected = 0U;
        radio_snapshot_view_t view = {0};
        err = read_selected(handle, key_a, key_b, kind, &selected, &view);
        if (err == ESP_OK) {
            *payload_size = view.payload_size;
            *generation = view.generation;
            if (view.payload_size > capacity) {
                err = ESP_ERR_INVALID_SIZE;
            } else if (view.payload_size != 0U) {
                memcpy(payload, view.payload, view.payload_size);
            }
        }
        nvs_close(handle);
    }
    memset(s_slot_a, 0, sizeof(s_slot_a));
    memset(s_slot_b, 0, sizeof(s_slot_b));
    unlock_store();
    return err;
}

esp_err_t radio_snapshot_store_write(
    const char *unit, uint32_t kind, const uint8_t *payload,
    size_t payload_size, radio_snapshot_store_write_report_t *report)
{
    if (report != NULL) {
        memset(report, 0, sizeof(*report));
        report->operation_error = ESP_OK;
    }
    if (!unit_is_valid(unit) || kind == 0U ||
        (payload == NULL && payload_size != 0U))
        return ESP_ERR_INVALID_ARG;
    if (payload_size > RADIO_SNAPSHOT_STORE_MAX_PAYLOAD_SIZE)
        return ESP_ERR_INVALID_SIZE;

    char key_a[16], key_b[16];
    esp_err_t err = make_slot_keys(unit, key_a, key_b);
    if (err != ESP_OK) return err;
    err = lock_store();
    if (err != ESP_OK) return err;

    nvs_handle_t handle = 0;
    err = open_state(NVS_READWRITE, &handle);
    if (err != ESP_OK) goto cleanup;

    unsigned active = 0U;
    radio_snapshot_view_t current = {0};
    const esp_err_t current_err = read_selected(
        handle, key_a, key_b, kind, &active, &current);
    uint32_t generation;
    unsigned target;
    if (current_err == ESP_ERR_NVS_NOT_FOUND) {
        generation = 1U;
        target = 0U;
    } else if (current_err == ESP_OK) {
        generation = next_generation(current.generation);
        target = active == 0U ? 1U : 0U;
    } else {
        err = current_err;
        nvs_close(handle);
        handle = 0;
        goto cleanup;
    }

    size_t encoded_size = 0U;
    uint8_t *target_record = target == 0U ? s_slot_a : s_slot_b;
    const radio_snapshot_result_t encode_result = radio_snapshot_encode(
        target_record, STORE_MAX_RECORD_SIZE, kind, generation, payload, payload_size,
        &encoded_size);
    if (encode_result != RADIO_SNAPSHOT_OK) {
        err = encode_result == RADIO_SNAPSHOT_NO_SPACE
                  ? ESP_ERR_INVALID_SIZE : ESP_ERR_INVALID_ARG;
        nvs_close(handle);
        handle = 0;
        goto cleanup;
    }

    const char *target_key = target == 0U ? key_a : key_b;
    esp_err_t operation_error = nvs_set_blob(
        handle, target_key, target_record, encoded_size);
    if (operation_error == ESP_OK)
        operation_error = RADIO_TEST_NVS_COMMIT(handle);
    nvs_close(handle);
    handle = 0;
    if (report != NULL) report->operation_error = operation_error;

    /* An error can still mean the new value reached flash. Reopen the NVS
       partition after failures so reconciliation reads flash, not NVS cache. */
    if (operation_error != ESP_OK) {
        const esp_err_t reopen_err = reopen_partition();
        if (reopen_err != ESP_OK) {
            err = reopen_err;
            if (report != NULL) report->uncertain = true;
            goto cleanup;
        }
    }

    err = open_state(NVS_READONLY, &handle);
    if (err != ESP_OK) {
        if (report != NULL) report->uncertain = true;
        goto cleanup;
    }
    unsigned selected = 0U;
    radio_snapshot_view_t recovered = {0};
    err = read_selected(handle, key_a, key_b, kind, &selected, &recovered);
    nvs_close(handle);
    handle = 0;
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND &&
            current_err == ESP_ERR_NVS_NOT_FOUND &&
            operation_error != ESP_OK) {
            err = operation_error;
            goto cleanup;
        }
        if (report != NULL) report->uncertain = true;
        goto cleanup;
    }

    if (selected_matches(&recovered, generation, payload, payload_size)) {
        err = ESP_OK;
        if (report != NULL) {
            report->generation = generation;
            report->reconciled_after_error = operation_error != ESP_OK;
        }
    } else if (operation_error != ESP_OK &&
               current_err == ESP_OK &&
               recovered.generation == current.generation) {
        err = operation_error;
        if (report != NULL) report->generation = current.generation;
    } else {
        err = operation_error == ESP_OK ? ESP_ERR_INVALID_STATE
                                        : operation_error;
        if (report != NULL) report->uncertain = true;
    }

cleanup:
    if (handle != 0) nvs_close(handle);
    memset(s_slot_a, 0, sizeof(s_slot_a));
    memset(s_slot_b, 0, sizeof(s_slot_b));
    unlock_store();
    return err;
}
