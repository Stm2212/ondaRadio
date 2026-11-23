#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "radio_catalog.h"
#include "radio_json.h"
#include "radio_lock_diagnostics.h"
#include "radio_test_adapters.h"
#include "radio_task_supervisor.h"
#include "radio_wifi.h"

#define CATALOG_RESULT_LIMIT RADIO_CATALOG_PAGE_SIZE
#define CATALOG_RESPONSE_MAX (64 * 1024)

typedef enum {
    CATALOG_SEARCH_POPULAR = 0,
    CATALOG_SEARCH_NAME,
    CATALOG_SEARCH_FILTER,
} catalog_search_kind_t;

typedef struct {
    char query[64];
    catalog_search_kind_t search_kind;
    radio_catalog_filter_t filter;
    size_t offset;
    uint32_t request_id;
} catalog_request_t;

typedef struct {
    char *buffer;
    size_t length;
    size_t capacity;
    bool overflow;
    bool accept_body;
} response_buffer_t;

static const char *TAG = "radio_catalog";
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_request_queue;
static bool s_initialized;
static radio_catalog_callback_t s_callback;
static uint32_t s_latest_request_id;

static void catalog_log_memory(const char *stage)
{
    const uint32_t internal_free = (uint32_t)heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t internal_largest = (uint32_t)heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t internal_minimum = (uint32_t)heap_caps_get_minimum_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t spiram_free = (uint32_t)heap_caps_get_free_size(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const uint32_t spiram_largest = (uint32_t)heap_caps_get_largest_free_block(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    ESP_LOGI(TAG, "%s: SRAM libre/mayor/minimo %u/%u/%u KiB, PSRAM libre/mayor %u/%u KiB, pila catalogo libre %u B",
             stage,
             (unsigned)(internal_free / 1024U),
             (unsigned)(internal_largest / 1024U),
             (unsigned)(internal_minimum / 1024U),
             (unsigned)(spiram_free / 1024U),
             (unsigned)(spiram_largest / 1024U),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static esp_err_t response_event_handler(esp_http_client_event_t *event)
{
    response_buffer_t *response = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_HEADERS_COMPLETE) {
        const int status_code = esp_http_client_get_status_code(event->client);
        response->accept_body = status_code >= 200 && status_code < 300;
        return ESP_OK;
    }
    /* Redirect and error bodies must not be joined to the final JSON. */
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0 ||
        !response->accept_body) {
        return ESP_OK;
    }

    const size_t incoming = (size_t)event->data_len;
    if (response->length >= response->capacity ||
        incoming > response->capacity - response->length - 1) {
        response->overflow = true;
        return ESP_FAIL;
    }

    memcpy(response->buffer + response->length, event->data, incoming);
    response->length += incoming;
    response->buffer[response->length] = '\0';
    return ESP_OK;
}

static bool copy_json_field(char *dst, size_t dst_size, const cJSON *object,
                            const char *field, bool required)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, field);
    if (item == NULL) return !required;
    if (!cJSON_IsString(item) || item->valuestring == NULL) return false;
    const size_t length = strnlen(item->valuestring, dst_size);
    if (length >= dst_size) return false;
    memcpy(dst, item->valuestring, length + 1U);
    return true;
}

static void url_encode(const char *input, char *output, size_t output_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)input; *p != '\0'; ++p) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            if (used + 1 >= output_size) break;
            output[used++] = (char)*p;
        } else {
            if (used + 3 >= output_size) break;
            output[used++] = '%';
            output[used++] = hex[*p >> 4];
            output[used++] = hex[*p & 0x0f];
        }
    }
    output[used] = '\0';
}

static void catalog_build_query_path(const catalog_request_t *request,
                                     char *path, size_t path_size)
{
    if (request->search_kind == CATALOG_SEARCH_POPULAR) {
        snprintf(path, path_size,
                 "/json/stations/search?hidebroken=true&codec=mp3&order=clickcount&reverse=true&offset=%u&limit=%d",
                 (unsigned int)request->offset, CATALOG_RESULT_LIMIT);
        return;
    }

    char encoded_value[192];
    url_encode(request->query, encoded_value, sizeof(encoded_value));
    const char *parameter = "name";
    if (request->search_kind == CATALOG_SEARCH_FILTER) {
        switch (request->filter) {
        case RADIO_CATALOG_FILTER_COUNTRY: parameter = "countrycode"; break;
        case RADIO_CATALOG_FILTER_GENRE: parameter = "tag"; break;
        case RADIO_CATALOG_FILTER_LANGUAGE: parameter = "language"; break;
        case RADIO_CATALOG_FILTER_CODEC: parameter = "codec"; break;
        default: break;
        }
    }

    const bool searching_codec = request->search_kind == CATALOG_SEARCH_FILTER &&
                                 request->filter == RADIO_CATALOG_FILTER_CODEC;
    if (searching_codec) {
        snprintf(path, path_size,
                 "/json/stations/search?%s=%s&hidebroken=true&order=clickcount&reverse=true&offset=%u&limit=%d",
                 parameter, encoded_value, (unsigned int)request->offset,
                 CATALOG_RESULT_LIMIT);
    } else {
        snprintf(path, path_size,
                 "/json/stations/search?%s=%s&hidebroken=true&codec=mp3&order=clickcount&reverse=true&offset=%u&limit=%d",
                 parameter, encoded_value, (unsigned int)request->offset,
                 CATALOG_RESULT_LIMIT);
    }
}

static esp_err_t catalog_fetch_page(const catalog_request_t *request,
                                    const char *query_path,
                                    response_buffer_t *response)
{
    /* A Radio Browser mirror can be unavailable independently of Wi-Fi. */
    static const char *const hosts[] = {
        "de2.api.radio-browser.info",
        "fi1.api.radio-browser.info",
        "de1.api.radio-browser.info",
    };
    esp_err_t result = ESP_FAIL;
    for (size_t attempt = 0; attempt < sizeof(hosts) / sizeof(hosts[0]); ++attempt) {
        const int64_t attempt_started_us = esp_timer_get_time();
        char url[512];
        snprintf(url, sizeof(url), "https://%s%s", hosts[attempt], query_path);
        response->length = 0;
        response->overflow = false;
        response->accept_body = false;
        response->buffer[0] = '\0';
        ESP_LOGD(TAG, "Consulta %lu: mirror %u",
                 (unsigned long)request->request_id, (unsigned)(attempt + 1U));
        const esp_http_client_config_t config = {
            .url = url,
            .event_handler = response_event_handler,
            .user_data = response,
            .timeout_ms = 10000,
            .buffer_size = 4096,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .disable_auto_redirect = false,
            .max_redirection_count = 5,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == NULL) return ESP_ERR_NO_MEM;

        esp_http_client_set_header(client, "User-Agent", "OndaPortatil/0.2 ESP-IDF");
        esp_http_client_set_header(client, "Accept", "application/json");
        result = RADIO_TEST_NETWORK_GATE();
        if (result == ESP_OK) result = esp_http_client_perform(client);
        const int status_code = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
        if (response->overflow) result = ESP_ERR_INVALID_SIZE;
        else if (result == ESP_OK && (status_code < 200 || status_code >= 300))
            result = ESP_FAIL;
        ESP_LOGI(TAG,
                 "catalog HTTP timing request=%lu mirror=%u elapsed_us=%lld result=%s http=%d bytes=%u",
                 (unsigned long)request->request_id, (unsigned)(attempt + 1U),
                 (long long)(esp_timer_get_time() - attempt_started_us),
                 esp_err_to_name(result), status_code,
                 (unsigned)response->length);
        if (result == ESP_OK) break;
        ESP_LOGW(TAG, "Fallo %s: %s, HTTP %d, %u bytes%s", hosts[attempt],
                 esp_err_to_name(result), status_code, (unsigned)response->length,
                 response->overflow ? " (respuesta demasiado grande)" : "");
        if (result == ESP_ERR_NO_MEM) break;
    }
    return result;
}

static esp_err_t catalog_parse_page(const response_buffer_t *response,
                                    radio_station_t *stations,
                                    size_t *station_count, size_t *response_count)
{
    cJSON *root = radio_json_parse_exact(response->buffer, response->length);
    if (root == NULL) return ESP_ERR_INVALID_RESPONSE;
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        if (*response_count >= CATALOG_RESULT_LIMIT) break;
        ++*response_count;
        if (!cJSON_IsObject(item)) continue;
        if (*station_count >= CATALOG_RESULT_LIMIT) break;
        radio_station_t decoded = {0};
        bool valid =
            copy_json_field(decoded.name, sizeof(decoded.name), item,
                            "name", true) &&
            copy_json_field(decoded.country, sizeof(decoded.country), item,
                            "country", false) &&
            copy_json_field(decoded.codec, sizeof(decoded.codec), item,
                            "codec", false) &&
            copy_json_field(decoded.uuid, sizeof(decoded.uuid), item,
                            "stationuuid", false) &&
            copy_json_field(decoded.url, sizeof(decoded.url), item,
                            "url_resolved", false);
        if (valid && decoded.url[0] == '\0') {
            valid = copy_json_field(decoded.url, sizeof(decoded.url), item,
                                    "url", true);
        }
        if (valid && decoded.name[0] != '\0' && decoded.url[0] != '\0') {
            stations[*station_count] = decoded;
            ++*station_count;
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static void catalog_process_request(const catalog_request_t *queued_request)
{
    const catalog_request_t request = *queued_request;
    const int64_t request_started_us = esp_timer_get_time();
    int64_t fetch_elapsed_us = 0;
    int64_t parse_elapsed_us = 0;

    radio_station_t *stations = NULL;
    size_t station_count = 0;
    size_t response_count = 0;
    esp_err_t result = ESP_FAIL;

    if (!radio_wifi_is_connected()) {
        result = ESP_ERR_INVALID_STATE;
        catalog_log_memory("Consulta cancelada: Wi-Fi desconectado");
        goto done;
    }

    catalog_log_memory("Antes de reservar resultados");
    stations = RADIO_TEST_HEAP_CAPS_CALLOC(
        CATALOG_RESULT_LIMIT, sizeof(radio_station_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    result = stations != NULL ? ESP_FAIL : ESP_ERR_NO_MEM;
    if (stations == NULL) {
        catalog_log_memory("Fallo al reservar resultados");
        goto done;
    }
    ESP_LOGI(TAG, "Buffer de estaciones: %u B (%u x %u B)",
             (unsigned)(CATALOG_RESULT_LIMIT * sizeof(radio_station_t)),
             (unsigned)CATALOG_RESULT_LIMIT, (unsigned)sizeof(radio_station_t));
    catalog_log_memory("Resultados reservados en PSRAM");

    response_buffer_t response = {
        .capacity = CATALOG_RESPONSE_MAX,
        .buffer = RADIO_TEST_HEAP_CAPS_MALLOC(
            CATALOG_RESPONSE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
    };
    if (response.buffer == NULL) {
        result = ESP_ERR_NO_MEM;
        catalog_log_memory("Fallo al reservar respuesta HTTP");
        goto done;
    }
    response.buffer[0] = '\0';
    ESP_LOGI(TAG, "Buffer de respuesta HTTP: %u B", (unsigned)response.capacity);
    catalog_log_memory("Respuesta HTTP reservada");

    char query_path[384];
    catalog_build_query_path(&request, query_path, sizeof(query_path));
    const int64_t fetch_started_us = esp_timer_get_time();
    result = catalog_fetch_page(&request, query_path, &response);
    fetch_elapsed_us = esp_timer_get_time() - fetch_started_us;
    if (result == ESP_OK) {
        const int64_t parse_started_us = esp_timer_get_time();
        result = catalog_parse_page(&response, stations, &station_count,
                                    &response_count);
        parse_elapsed_us = esp_timer_get_time() - parse_started_us;
    }
    catalog_log_memory("Parseo terminado, respuesta aun reservada");
    heap_caps_free(response.buffer);
    catalog_log_memory("Respuesta HTTP liberada");

done:
    ESP_LOGI(TAG,
             "catalog request timing request=%lu fetch_us=%lld parse_us=%lld total_us=%lld result=%s stations=%u response=%u",
             (unsigned long)request.request_id, (long long)fetch_elapsed_us,
             (long long)parse_elapsed_us,
             (long long)(esp_timer_get_time() - request_started_us),
             esp_err_to_name(result), (unsigned)station_count,
             (unsigned)response_count);
    radio_catalog_callback_t callback = NULL;
    (void)radio_mutex_take_audited(s_lock, portMAX_DELAY, TAG,
                                   "catalog", __func__);
    if (request.request_id == s_latest_request_id) {
        callback = s_callback;
    } else {
        ESP_LOGI(TAG, "Se descarta respuesta obsoleta %lu",
                 (unsigned long)request.request_id);
    }
    xSemaphoreGive(s_lock);
    if (callback != NULL) {
        callback(stations, station_count, request.offset,
                 request.offset + response_count,
                 result == ESP_OK && response_count == CATALOG_RESULT_LIMIT,
                 request.request_id, result);
    }
    heap_caps_free(stations);
    catalog_log_memory("Resultados liberados");
}

static void catalog_worker(void *arg)
{
    (void)arg;
    radio_task_supervisor_register(RADIO_SUPERVISED_TASK_CATALOG);
    /* A one-slot command queue bounds concurrent catalog and JSON work. */
    for (;;) {
        catalog_request_t request;
        if (xQueueReceive(s_request_queue, &request,
                          pdMS_TO_TICKS(1000)) != pdTRUE) {
            radio_task_supervisor_progress(RADIO_SUPERVISED_TASK_CATALOG);
            continue;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool current = request.request_id == s_latest_request_id;
        xSemaphoreGive(s_lock);
        if (current) {
            catalog_process_request(&request);
            catalog_log_memory("Fin tarea catalogo");
        }
        radio_task_supervisor_progress(RADIO_SUPERVISED_TASK_CATALOG);
    }
}

esp_err_t radio_catalog_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_request_queue = xQueueCreate(1, sizeof(catalog_request_t));
    if (s_request_queue == NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(catalog_worker, "radio_catalog", 12288, NULL, 4, NULL) != pdPASS) {
        vQueueDelete(s_request_queue);
        vSemaphoreDelete(s_lock);
        s_request_queue = NULL;
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    return ESP_OK;
}

static esp_err_t start_request(const char *query, catalog_search_kind_t search_kind,
                               radio_catalog_filter_t filter, size_t offset,
                               uint32_t request_id)
{
    if (!s_initialized || request_id == 0 ||
        offset > UINT_MAX - RADIO_CATALOG_PAGE_SIZE ||
        (search_kind != CATALOG_SEARCH_POPULAR &&
         (query == NULL || query[0] == '\0')) ||
        (search_kind == CATALOG_SEARCH_FILTER &&
         filter > RADIO_CATALOG_FILTER_CODEC)) {
        return s_initialized ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    }
    if (!radio_wifi_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    catalog_request_t request = {
        .search_kind = search_kind,
        .filter = filter,
        .offset = offset,
        .request_id = request_id,
    };
    if (query != NULL) {
        const size_t query_length = strnlen(query, sizeof(request.query));
        if (query_length >= sizeof(request.query)) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(request.query, query, query_length + 1);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_latest_request_id = request_id;
    const BaseType_t queued = RADIO_TEST_QUEUE_OVERWRITE(s_request_queue,
                                                         &request);
    xSemaphoreGive(s_lock);
    if (queued != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t radio_catalog_load_popular(size_t offset, uint32_t request_id)
{
    return start_request(NULL, CATALOG_SEARCH_POPULAR,
                         RADIO_CATALOG_FILTER_COUNTRY, offset, request_id);
}

esp_err_t radio_catalog_search_name(const char *name, size_t offset,
                                    uint32_t request_id)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    return start_request(name, CATALOG_SEARCH_NAME,
                         RADIO_CATALOG_FILTER_COUNTRY, offset, request_id);
}

esp_err_t radio_catalog_search_filter(radio_catalog_filter_t filter,
                                      const char *value, size_t offset,
                                      uint32_t request_id)
{
    if (value == NULL || value[0] == '\0' ||
        filter > RADIO_CATALOG_FILTER_CODEC) return ESP_ERR_INVALID_ARG;
    return start_request(value, CATALOG_SEARCH_FILTER, filter, offset,
                         request_id);
}

void radio_catalog_set_callback(radio_catalog_callback_t callback)
{
    if (!s_initialized) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_callback = callback;
    xSemaphoreGive(s_lock);
}
