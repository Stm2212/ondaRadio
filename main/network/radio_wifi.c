#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "radio_wifi.h"
#include "radio_generation.h"
#include "radio_json.h"
#include "radio_lock_diagnostics.h"
#include "radio_station_json.h"
#include "radio_station_store.h"
#include "radio_url_codec.h"
#include "radio_wifi_form.h"
#include "radio_wifi_credentials_store.h"
#include "radio_nvs_worker.h"
#include "radio_test_adapters.h"
#include "radio_task_supervisor.h"

/* UTF-8 pages are embedded and NUL-terminated by ESP-IDF's EMBED_TXTFILES.
   The station manager is sent as two chunks to keep its response bounded. */
extern const char s_portal_page[] asm("_binary_onda_wifi_html_start");
extern const char s_station_manager_page_start[] asm("_binary_onda_stations_head_html_start");
extern const char s_station_manager_page_script[] asm("_binary_onda_stations_script_html_start");
extern const char s_connecting_page[] asm("_binary_onda_connecting_html_start");

#define WIFI_NVS_NAMESPACE "radio_cfg"
#define WIFI_NVS_SCHEMA_VERSION_KEY "schema_ver"
#define WIFI_NVS_SCHEMA_VERSION 1U
#define WIFI_PORTAL_PASSWORD_KEY "portal_pass"
#define WIFI_PORTAL_PASSWORD_BYTES 16U
#define WIFI_PORTAL_PASSWORD_LENGTH (WIFI_PORTAL_PASSWORD_BYTES * 2U)
#define PORTAL_SESSION_COUNT 2U
#define PORTAL_SESSION_TOKEN_BYTES 16U
#define PORTAL_SESSION_TOKEN_LENGTH (PORTAL_SESSION_TOKEN_BYTES * 2U)
#define PORTAL_SESSION_IDLE_MS (15U * 60U * 1000U)
#define WIFI_CANDIDATE_TIMEOUT_MS 20000U
#define PORTAL_SHUTDOWN_DELAY_MS 18000
#define FORM_BODY_MAX 512
#define WIFI_SCAN_RESULT_LIMIT 32
#define STATION_QUERY_MAX 96
#define STATION_RESPONSE_MAX (64 * 1024)
#define STATION_SEARCH_PAGE_SIZE 20
#define STATION_SEARCH_MAX_OFFSET 180
#define STATION_SEARCH_JOB_COUNT 2U
#define STATION_SEARCH_QUEUE_LENGTH 2U
#define STATION_SEARCH_RESULT_TTL_MS (60U * 1000U)
#define STATION_SEARCH_TIMEOUT_MS 12000U
#define STATION_SEARCH_POLL_MS 50U
#define STATION_POST_MAX 1536
#define PORTAL_MUTATION_COUNT 4U
#define PORTAL_MUTATION_RESULT_TTL_MS (60U * 1000U)

static const char *TAG = "radio_wifi";
/* Protects the Wi-Fi manager state; never held across driver calls or callbacks. */
static SemaphoreHandle_t s_state_lock;
/* Serializes portal start/stop; never acquired from HTTP handlers. */
static SemaphoreHandle_t s_portal_lifecycle_lock;
static bool s_initialized;
static bool s_sntp_initialized;
static bool s_connected;
static bool s_station_connecting;
static bool s_station_reconnect_paused;
static bool s_station_retry_scheduled;
static uint8_t s_station_retry_failures;
static bool s_portal_active;
static uint32_t s_status_generation;
typedef struct {
    bool active;
    char session_id[PORTAL_SESSION_TOKEN_LENGTH + 1U];
    char csrf_token[PORTAL_SESSION_TOKEN_LENGTH + 1U];
    TickType_t last_activity;
} portal_session_t;
typedef struct {
    bool active;
    uint32_t generation;
    bool configuration_ready;
    bool link_down;
    bool attempt_started;
    bool commit_pending;
    bool commit_link_connected;
    bool authmode_detected;
    wifi_auth_mode_t authmode;
    char candidate_ssid[33];
    char candidate_password[65];
    wifi_config_t previous_config;
    esp_netif_ip_info_t acquired_ip;
} wifi_candidate_t;
typedef enum {
    STATION_SEARCH_FREE = 0,
    STATION_SEARCH_QUEUED,
    STATION_SEARCH_RUNNING,
    STATION_SEARCH_READY,
    STATION_SEARCH_FAILED,
    STATION_SEARCH_CANCELLED,
} station_search_state_t;
typedef struct {
    size_t slot;
    uint32_t generation;
} station_search_queue_item_t;
typedef struct {
    bool used;
    uint32_t generation;
    station_search_state_t state;
    bool cancel_requested;
    char id[PORTAL_SESSION_TOKEN_LENGTH + 1U];
    char owner_session[PORTAL_SESSION_TOKEN_LENGTH + 1U];
    char filter[24];
    char query[STATION_QUERY_MAX];
    char codec[8];
    uint16_t offset;
    char *response_body;
    size_t response_length;
    esp_err_t error;
    uint8_t readers;
    TickType_t last_activity;
} station_search_job_t;
typedef struct {
    bool used;
    bool complete;
    uint32_t generation;
    char id[PORTAL_SESSION_TOKEN_LENGTH + 1U];
    char owner_session[PORTAL_SESSION_TOKEN_LENGTH + 1U];
    esp_err_t result;
    bool is_favorite;
    TickType_t completed_at;
} portal_mutation_t;
typedef struct {
    size_t slot;
    uint32_t generation;
} portal_mutation_context_t;
typedef struct {
    char *buffer;
    size_t length;
    bool overflow;
    bool accept_body;
    size_t slot;
    uint32_t generation;
    bool cancelled;
} station_response_t;
static portal_session_t s_portal_sessions[PORTAL_SESSION_COUNT];
static wifi_candidate_t s_wifi_candidate;
static station_search_job_t s_station_search_jobs[STATION_SEARCH_JOB_COUNT];
static portal_mutation_t s_portal_mutations[PORTAL_MUTATION_COUNT];
static uint32_t s_wifi_candidate_generation;
static uint32_t s_station_search_generation;
static uint32_t s_portal_mutation_generation;
static bool s_portal_credentials_submitted;
static bool s_portal_shutdown_scheduled;
static uint32_t s_portal_shutdown_generation;
static uint8_t s_last_disconnect_reason;
static uint8_t s_disconnect_count;
static esp_err_t s_last_connect_error;
static wifi_auth_mode_t s_authmode = WIFI_AUTH_OPEN;
static bool s_authmode_stored;
static esp_netif_t *s_ap_netif;
static QueueHandle_t s_station_search_queue;
static TaskHandle_t s_station_search_worker;
static char s_ssid[33];
static char s_portal_ssid[33];
static char s_portal_password[WIFI_PORTAL_PASSWORD_LENGTH + 1U];
static radio_wifi_status_cb_t s_status_callback;
static httpd_handle_t s_http_server;
static bool s_http_server_ready;

static void schedule_station_retry(void);
static void rollback_wifi_candidate(esp_err_t reason, uint32_t generation);
static void begin_candidate_connection(void);
static void station_search_worker_task(void *arg);
static esp_err_t read_station_json(httpd_req_t *request, char *body,
                                    size_t capacity, size_t *body_length);
static esp_err_t save_credentials_job(void *context);

static void start_time_sync(void)
{
    setenv("TZ", "ART3", 1);
    tzset();
    if (s_sntp_initialized) return;

    const esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    const esp_err_t err = esp_netif_sntp_init(&config);
    if (err == ESP_OK) {
        s_sntp_initialized = true;
        ESP_LOGI(TAG, "Reloj sincronizando por SNTP; zona horaria Argentina (UTC-3)");
    } else {
        ESP_LOGW(TAG, "No se pudo iniciar la sincronizacion horaria: %s",
                 esp_err_to_name(err));
    }
}

static void state_lock(void)
{
    if (s_state_lock != NULL) {
        (void)radio_mutex_take_audited(s_state_lock, portMAX_DELAY, TAG,
                                       "wifi_state", __func__);
    }
}

static void state_unlock(void)
{
    if (s_state_lock != NULL) {
        xSemaphoreGive(s_state_lock);
    }
}

/* Call while holding s_state_lock when runtime state exposed in a snapshot changes. */
static void status_generation_advance_locked(void)
{
    s_status_generation = radio_generation_next(s_status_generation);
}

static void secure_clear(void *memory, size_t length)
{
    volatile uint8_t *bytes = (volatile uint8_t *)memory;
    while (length-- > 0U) *bytes++ = 0U;
}

static void get_info_locked(radio_wifi_info_t *info)
{
    info->status_generation = s_status_generation;
    info->connected = s_connected;
    info->provisioning_active = s_portal_active;
    snprintf(info->ssid, sizeof(info->ssid), "%s", s_ssid);
    snprintf(info->provisioning_ssid, sizeof(info->provisioning_ssid), "%s",
             s_portal_ssid);
}

static void notify_status(void)
{
    radio_wifi_info_t info = {0};
    state_lock();
    radio_wifi_status_cb_t callback = s_status_callback;
    get_info_locked(&info);
    state_unlock();
    if (callback != NULL) {
        callback(&info);
    }
}

static bool request_is_on_softap(httpd_req_t *request)
{
    if (request == NULL || s_ap_netif == NULL) return false;

    struct sockaddr_storage local_address = {0};
    socklen_t address_length = sizeof(local_address);
    if (getsockname(httpd_req_to_sockfd(request),
                    (struct sockaddr *)&local_address,
                    &address_length) != 0) {
        return false;
    }

    /* With LWIP IPv6 enabled the HTTP server listens on a dual-stack IPv6
       socket, so IPv4 clients are reported as IPv4-mapped IPv6 addresses. */
    uint32_t local_ipv4 = 0;
    if (local_address.ss_family == AF_INET) {
        local_ipv4 = ((const struct sockaddr_in *)&local_address)
                         ->sin_addr.s_addr;
    } else if (local_address.ss_family == AF_INET6) {
        const struct in6_addr *address =
            &((const struct sockaddr_in6 *)&local_address)->sin6_addr;
        static const uint8_t v4_mapped_prefix[12] = {
            0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0xffU, 0xffU};
        if (memcmp(address->s6_addr, v4_mapped_prefix,
                   sizeof(v4_mapped_prefix)) != 0) {
            return false;
        }
        memcpy(&local_ipv4, address->s6_addr + sizeof(v4_mapped_prefix),
               sizeof(local_ipv4));
    } else {
        return false;
    }

    esp_netif_ip_info_t ap_ip = {0};
    if (esp_netif_get_ip_info(s_ap_netif, &ap_ip) != ESP_OK ||
        ap_ip.ip.addr == 0) {
        return false;
    }
    return local_ipv4 == ap_ip.ip.addr;
}

static esp_err_t reject_portal_request(httpd_req_t *request,
                                      const char *status,
                                      const char *message)
{
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_send(request, message, HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
}

static esp_err_t require_softap_request(httpd_req_t *request)
{
    if (request_is_on_softap(request)) return ESP_OK;
    return reject_portal_request(request, "403 Forbidden",
                                 "Portal disponible solo por el punto de acceso local");
}

static bool constant_time_string_equal(const char *expected,
                                       const char *actual,
                                       size_t expected_length)
{
    if (expected == NULL || actual == NULL) return false;
    const size_t actual_length = strnlen(actual, expected_length + 1U);
    unsigned difference = (unsigned)(actual_length ^ expected_length);
    for (size_t i = 0; i < expected_length; ++i) {
        const unsigned char candidate = i < actual_length
                                            ? (unsigned char)actual[i]
                                            : 0U;
        difference |= (unsigned char)expected[i] ^ candidate;
    }
    return difference == 0U;
}

static bool is_lowercase_hex_token(const char *value, size_t length)
{
    if (value == NULL || length != PORTAL_SESSION_TOKEN_LENGTH) return false;
    for (size_t i = 0; i < length; ++i) {
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool get_session_id_from_cookie(httpd_req_t *request,
                                       char session_id[PORTAL_SESSION_TOKEN_LENGTH + 1U])
{
    char cookie_header[192] = {0};
    const size_t header_length = httpd_req_get_hdr_value_len(request, "Cookie");
    if (header_length == 0U || header_length >= sizeof(cookie_header) ||
        httpd_req_get_hdr_value_str(request, "Cookie", cookie_header,
                                    sizeof(cookie_header)) != ESP_OK) {
        secure_clear(cookie_header, sizeof(cookie_header));
        return false;
    }

    bool found = false;
    bool valid = true;
    const char *cursor = cookie_header;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == ';' || *cursor == '\t') ++cursor;
        const char *end = strchr(cursor, ';');
        if (end == NULL) end = cursor + strlen(cursor);
        const char *equals = memchr(cursor, '=', (size_t)(end - cursor));
        if (equals != NULL &&
            (size_t)(equals - cursor) == sizeof("onda_session") - 1U &&
            memcmp(cursor, "onda_session", sizeof("onda_session") - 1U) == 0) {
            const size_t value_length = (size_t)(end - equals - 1);
            if (found || !is_lowercase_hex_token(equals + 1, value_length)) {
                valid = false;
                break;
            }
            memcpy(session_id, equals + 1, value_length);
            session_id[value_length] = '\0';
            found = true;
        }
        cursor = *end == ';' ? end + 1 : end;
    }
    secure_clear(cookie_header, sizeof(cookie_header));
    if (!valid) secure_clear(session_id, PORTAL_SESSION_TOKEN_LENGTH + 1U);
    return found && valid;
}

static bool get_csrf_header(httpd_req_t *request,
                            char csrf_token[PORTAL_SESSION_TOKEN_LENGTH + 1U])
{
    const size_t header_length =
        httpd_req_get_hdr_value_len(request, "X-CSRF-Token");
    return header_length == PORTAL_SESSION_TOKEN_LENGTH &&
           httpd_req_get_hdr_value_str(request, "X-CSRF-Token", csrf_token,
                                       PORTAL_SESSION_TOKEN_LENGTH + 1U) == ESP_OK &&
           is_lowercase_hex_token(csrf_token, PORTAL_SESSION_TOKEN_LENGTH);
}

static bool request_has_json_content_type(httpd_req_t *request)
{
    char content_type[64] = {0};
    const size_t header_length =
        httpd_req_get_hdr_value_len(request, "Content-Type");
    if (header_length == 0U || header_length >= sizeof(content_type) ||
        httpd_req_get_hdr_value_str(request, "Content-Type", content_type,
                                    sizeof(content_type)) != ESP_OK) {
        return false;
    }
    static const char json_type[] = "application/json";
    return strncmp(content_type, json_type, sizeof(json_type) - 1U) == 0 &&
           (content_type[sizeof(json_type) - 1U] == '\0' ||
            content_type[sizeof(json_type) - 1U] == ';');
}

static void random_token_hex(char token[PORTAL_SESSION_TOKEN_LENGTH + 1U])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t random_bytes[PORTAL_SESSION_TOKEN_BYTES] = {0};
    esp_fill_random(random_bytes, sizeof(random_bytes));
    for (size_t i = 0; i < sizeof(random_bytes); ++i) {
        token[i * 2U] = hex[random_bytes[i] >> 4U];
        token[i * 2U + 1U] = hex[random_bytes[i] & 0x0fU];
    }
    token[PORTAL_SESSION_TOKEN_LENGTH] = '\0';
    memset(random_bytes, 0, sizeof(random_bytes));
}

static void expire_portal_sessions_locked(TickType_t now)
{
    const TickType_t idle_timeout = pdMS_TO_TICKS(PORTAL_SESSION_IDLE_MS);
    for (size_t i = 0; i < PORTAL_SESSION_COUNT; ++i) {
        portal_session_t *session = &s_portal_sessions[i];
        if (session->active &&
            (TickType_t)(now - session->last_activity) >= idle_timeout) {
            memset(session, 0, sizeof(*session));
        }
    }
}

static void clear_portal_sessions_locked(void)
{
    memset(s_portal_sessions, 0, sizeof(s_portal_sessions));
    secure_clear(s_portal_mutations, sizeof(s_portal_mutations));
}

/* Fills the caller-provided cookie buffer with the Set-Cookie value. The
   buffer must stay alive until the response is sent: httpd_resp_set_hdr keeps
   the pointer and only serializes it inside httpd_resp_send. */
static esp_err_t portal_session_open_from_softap(httpd_req_t *request,
                                                 char *cookie,
                                                 size_t cookie_size)
{
    if (cookie == NULL || cookie_size == 0U) return ESP_ERR_INVALID_ARG;
    cookie[0] = '\0';
    char presented_id[PORTAL_SESSION_TOKEN_LENGTH + 1U] = {0};
    char generated_id[PORTAL_SESSION_TOKEN_LENGTH + 1U] = {0};
    char generated_csrf[PORTAL_SESSION_TOKEN_LENGTH + 1U] = {0};
    const bool has_presented_id =
        get_session_id_from_cookie(request, presented_id);

    random_token_hex(generated_id);
    random_token_hex(generated_csrf);

    bool available = false;
    const TickType_t now = xTaskGetTickCount();
    state_lock();
    if (!s_portal_active) {
        state_unlock();
        secure_clear(presented_id, sizeof(presented_id));
        secure_clear(generated_id, sizeof(generated_id));
        secure_clear(generated_csrf, sizeof(generated_csrf));
        return reject_portal_request(request, "403 Forbidden",
                                     "El portal local no está activo");
    }
    expire_portal_sessions_locked(now);

    portal_session_t *slot = NULL;
    if (has_presented_id) {
        for (size_t i = 0; i < PORTAL_SESSION_COUNT; ++i) {
            if (s_portal_sessions[i].active &&
                constant_time_string_equal(s_portal_sessions[i].session_id,
                                            presented_id,
                                            PORTAL_SESSION_TOKEN_LENGTH)) {
                slot = &s_portal_sessions[i];
                break;
            }
        }
    }

    if (slot != NULL) {
        slot->last_activity = now;
        snprintf(cookie, cookie_size,
                 "onda_session=%s; Path=/; HttpOnly; SameSite=Strict",
                 slot->session_id);
        available = true;
    } else {
        for (size_t i = 0; i < PORTAL_SESSION_COUNT; ++i) {
            if (!s_portal_sessions[i].active) {
                slot = &s_portal_sessions[i];
                break;
            }
        }
        if (slot != NULL) {
            slot->active = true;
            memcpy(slot->session_id, generated_id, sizeof(slot->session_id));
            memcpy(slot->csrf_token, generated_csrf, sizeof(slot->csrf_token));
            slot->last_activity = now;
            snprintf(cookie, cookie_size,
                     "onda_session=%s; Path=/; HttpOnly; SameSite=Strict",
                     slot->session_id);
            available = true;
        }
    }
    state_unlock();

    secure_clear(presented_id, sizeof(presented_id));
    secure_clear(generated_id, sizeof(generated_id));
    secure_clear(generated_csrf, sizeof(generated_csrf));
    if (!available) {
        secure_clear(cookie, cookie_size);
        return reject_portal_request(request, "503 Service Unavailable",
                                     "No hay sesiones disponibles; intentá más tarde");
    }
    return ESP_OK;
}

static esp_err_t authorize_portal_request(httpd_req_t *request,
                                          bool require_csrf,
                                          char *csrf_out,
                                          size_t csrf_out_size)
{
    if (require_softap_request(request) != ESP_OK) return ESP_FAIL;

    char session_id[PORTAL_SESSION_TOKEN_LENGTH + 1U] = {0};
    char csrf_token[PORTAL_SESSION_TOKEN_LENGTH + 1U] = {0};
    if (!get_session_id_from_cookie(request, session_id)) {
        return reject_portal_request(request, "401 Unauthorized",
                                     "Autenticación requerida");
    }
    if (require_csrf && !get_csrf_header(request, csrf_token)) {
        return reject_portal_request(request, "403 Forbidden",
                                     "Autorización de solicitud inválida");
    }

    bool session_found = false;
    bool csrf_valid = !require_csrf;
    const TickType_t now = xTaskGetTickCount();
    state_lock();
    expire_portal_sessions_locked(now);
    for (size_t i = 0; i < PORTAL_SESSION_COUNT; ++i) {
        portal_session_t *session = &s_portal_sessions[i];
        if (session->active &&
            constant_time_string_equal(session->session_id, session_id,
                                        PORTAL_SESSION_TOKEN_LENGTH)) {
            session_found = true;
            csrf_valid = !require_csrf ||
                         constant_time_string_equal(session->csrf_token,
                                                    csrf_token,
                                                    PORTAL_SESSION_TOKEN_LENGTH);
            if (csrf_valid) {
                session->last_activity = now;
                if (csrf_out != NULL &&
                    csrf_out_size >= sizeof(session->csrf_token)) {
                    memcpy(csrf_out, session->csrf_token,
                           sizeof(session->csrf_token));
                }
            }
            break;
        }
    }
    state_unlock();
    memset(session_id, 0, sizeof(session_id));
    memset(csrf_token, 0, sizeof(csrf_token));

    if (!session_found) {
        return reject_portal_request(request, "401 Unauthorized",
                                     "Sesión vencida o inválida");
    }
    if (!csrf_valid) {
        return reject_portal_request(request, "403 Forbidden",
                                     "Autorización de solicitud inválida");
    }
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    return ESP_OK;
}

static void portal_mutation_complete(esp_err_t result, bool is_favorite,
                                     void *context)
{
    portal_mutation_context_t *mutation_context = context;
    if (mutation_context == NULL) return;
    state_lock();
    if (mutation_context->slot < PORTAL_MUTATION_COUNT) {
        portal_mutation_t *mutation =
            &s_portal_mutations[mutation_context->slot];
        if (mutation->used &&
            mutation->generation == mutation_context->generation) {
            mutation->result = result;
            mutation->is_favorite = is_favorite;
            mutation->complete = true;
            mutation->completed_at = xTaskGetTickCount();
        }
    }
    state_unlock();
    free(mutation_context);
}

static void portal_mutation_discard(portal_mutation_context_t *context)
{
    if (context == NULL) return;
    state_lock();
    if (context->slot < PORTAL_MUTATION_COUNT) {
        portal_mutation_t *mutation = &s_portal_mutations[context->slot];
        if (mutation->used && mutation->generation == context->generation) {
            secure_clear(mutation, sizeof(*mutation));
        }
    }
    state_unlock();
    free(context);
}

static esp_err_t portal_mutation_begin(
    const char *owner_session,
    char id[PORTAL_SESSION_TOKEN_LENGTH + 1U],
    portal_mutation_context_t **context_out)
{
    if (owner_session == NULL || id == NULL || context_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portal_mutation_context_t *context = calloc(1, sizeof(*context));
    if (context == NULL) return ESP_ERR_NO_MEM;
    random_token_hex(id);

    const TickType_t now = xTaskGetTickCount();
    size_t slot = PORTAL_MUTATION_COUNT;
    state_lock();
    for (size_t i = 0; i < PORTAL_MUTATION_COUNT; ++i) {
        portal_mutation_t *mutation = &s_portal_mutations[i];
        if (mutation->used && mutation->complete &&
            now - mutation->completed_at >=
                pdMS_TO_TICKS(PORTAL_MUTATION_RESULT_TTL_MS)) {
            secure_clear(mutation, sizeof(*mutation));
        }
        if (!mutation->used && slot == PORTAL_MUTATION_COUNT) slot = i;
    }
    if (slot < PORTAL_MUTATION_COUNT) {
        portal_mutation_t *mutation = &s_portal_mutations[slot];
        if (++s_portal_mutation_generation == 0U) {
            ++s_portal_mutation_generation;
        }
        mutation->used = true;
        mutation->generation = s_portal_mutation_generation;
        snprintf(mutation->id, sizeof(mutation->id), "%s", id);
        snprintf(mutation->owner_session, sizeof(mutation->owner_session),
                 "%s", owner_session);
        context->slot = slot;
        context->generation = mutation->generation;
    }
    state_unlock();
    if (slot == PORTAL_MUTATION_COUNT) {
        secure_clear(context, sizeof(*context));
        free(context);
        secure_clear(id, PORTAL_SESSION_TOKEN_LENGTH + 1U);
        return ESP_ERR_NO_MEM;
    }
    *context_out = context;
    return ESP_OK;
}

static esp_err_t portal_mutation_send_status(
    httpd_req_t *request, const char *id, const char *owner_session)
{
    bool found = false;
    bool complete = false;
    bool is_favorite = false;
    esp_err_t result = ESP_OK;
    state_lock();
    for (size_t i = 0; i < PORTAL_MUTATION_COUNT; ++i) {
        const portal_mutation_t *mutation = &s_portal_mutations[i];
        if (mutation->used &&
            constant_time_string_equal(mutation->id, id,
                                        PORTAL_SESSION_TOKEN_LENGTH)) {
            if (constant_time_string_equal(mutation->owner_session,
                                            owner_session,
                                            PORTAL_SESSION_TOKEN_LENGTH)) {
                found = true;
                complete = mutation->complete;
                result = mutation->result;
                is_favorite = mutation->is_favorite;
            }
            break;
        }
    }
    state_unlock();
    if (!found) {
        return reject_portal_request(request, "404 Not Found",
                                     "Operación inexistente o vencida");
    }
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (!complete) {
        return httpd_resp_send(request, "{\"state\":\"pending\"}",
                               HTTPD_RESP_USE_STRLEN);
    }
    const char *error_code = result == ESP_ERR_NO_MEM ? "full" :
                             result == ESP_ERR_INVALID_STATE ? "edge" :
                             result == ESP_ERR_NOT_FOUND ? "not_found" :
                             result == ESP_OK ? "" : "storage";
    char response[112];
    if (result == ESP_OK) {
        snprintf(response, sizeof(response),
                 "{\"state\":\"done\",\"ok\":true,\"favorite\":%s}",
                 is_favorite ? "true" : "false");
    } else {
        snprintf(response, sizeof(response),
                 "{\"state\":\"done\",\"ok\":false,\"error\":\"%s\"}",
                 error_code);
    }
    return httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t portal_csrf_handler(httpd_req_t *request)
{
    char csrf_token[PORTAL_SESSION_TOKEN_LENGTH + 1U] = {0};
    const esp_err_t auth_err = authorize_portal_request(request, false,
                                                        csrf_token,
                                                        sizeof(csrf_token));
    if (auth_err != ESP_OK) return auth_err;
    char response[80];
    snprintf(response, sizeof(response), "{\"csrf\":\"%s\"}", csrf_token);
    secure_clear(csrf_token, sizeof(csrf_token));
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    const esp_err_t send_err =
        httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
    secure_clear(response, sizeof(response));
    return send_err;
}

static esp_err_t portal_logout_handler(httpd_req_t *request)
{
    if (authorize_portal_request(request, true, NULL, 0) != ESP_OK) {
        return ESP_FAIL;
    }

    char session_id[PORTAL_SESSION_TOKEN_LENGTH + 1U] = {0};
    if (!get_session_id_from_cookie(request, session_id)) {
        return reject_portal_request(request, "401 Unauthorized",
                                     "Sesión vencida o inválida");
    }

    bool revoked = false;
    state_lock();
    for (size_t i = 0; i < PORTAL_SESSION_COUNT; ++i) {
        portal_session_t *session = &s_portal_sessions[i];
        if (session->active &&
            constant_time_string_equal(session->session_id, session_id,
                                        PORTAL_SESSION_TOKEN_LENGTH)) {
            secure_clear(session, sizeof(*session));
            revoked = true;
            break;
        }
    }
    if (revoked) {
        for (size_t i = 0; i < STATION_SEARCH_JOB_COUNT; ++i) {
            station_search_job_t *job = &s_station_search_jobs[i];
            if (!job->used ||
                !constant_time_string_equal(job->owner_session, session_id,
                                            PORTAL_SESSION_TOKEN_LENGTH)) {
                continue;
            }
            if (job->state == STATION_SEARCH_QUEUED) {
                job->cancel_requested = true;
                job->state = STATION_SEARCH_CANCELLED;
                job->error = ESP_ERR_INVALID_STATE;
                job->last_activity = xTaskGetTickCount();
            } else if (job->state == STATION_SEARCH_RUNNING) {
                job->cancel_requested = true;
                job->last_activity = xTaskGetTickCount();
            } else if (job->readers == 0U) {
                free(job->response_body);
                secure_clear(job, sizeof(*job));
            }
        }
        for (size_t i = 0; i < PORTAL_MUTATION_COUNT; ++i) {
            portal_mutation_t *mutation = &s_portal_mutations[i];
            if (mutation->used &&
                constant_time_string_equal(mutation->owner_session,
                                            session_id,
                                            PORTAL_SESSION_TOKEN_LENGTH)) {
                secure_clear(mutation, sizeof(*mutation));
            }
        }
    }
    state_unlock();
    secure_clear(session_id, sizeof(session_id));
    if (!revoked) {
        return reject_portal_request(request, "401 Unauthorized",
                                     "Sesión vencida o inválida");
    }

    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Set-Cookie",
                       "onda_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
    return httpd_resp_send(request, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static const char *wifi_auth_label(wifi_auth_mode_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN: return "Abierta";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    case WIFI_AUTH_OWE: return "WPA3 Enhanced Open";
    case WIFI_AUTH_ENTERPRISE: return "WPA2 empresarial";
    case WIFI_AUTH_WPA3_ENTERPRISE: return "WPA3 empresarial";
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE: return "WPA2/WPA3 empresarial";
    case WIFI_AUTH_WPA3_ENT_192: return "WPA3 empresarial 192";
    default: return "Seguridad no identificada";
    }
}

static bool authmode_is_usable(wifi_auth_mode_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN:
    case WIFI_AUTH_WPA_PSK:
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK:
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:
    case WIFI_AUTH_OWE:
        return true;
    default:
        /* Enterprise and WEP require credentials/settings this portal does
           not collect; don't offer a connection mode we cannot complete. */
        return false;
    }
}

static esp_err_t detect_authmode(const char *ssid, wifi_auth_mode_t *authmode)
{
    if (ssid == NULL || ssid[0] == '\0' || authmode == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_scan_config_t scan_config = {
        .ssid = (uint8_t *)ssid,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = 100, .max = 150},
        .show_hidden = true,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo detectar seguridad de %s: %s", ssid,
                 esp_err_to_name(err));
        return err;
    }

    uint16_t count = 0;
    err = esp_wifi_scan_get_ap_num(&count);
    if (err != ESP_OK || count == 0) {
        esp_wifi_clear_ap_list();
        return err == ESP_OK ? ESP_ERR_NOT_FOUND : err;
    }

    wifi_ap_record_t *records = calloc(count, sizeof(*records));
    if (records == NULL) {
        esp_wifi_clear_ap_list();
        return ESP_ERR_NO_MEM;
    }
    uint16_t record_count = count;
    err = esp_wifi_scan_get_ap_records(&record_count, records);
    if (err == ESP_OK) {
        int best_rssi = -128;
        bool found = false;
        for (uint16_t i = 0; i < record_count; ++i) {
            if (strncmp((const char *)records[i].ssid, ssid,
                        sizeof(records[i].ssid)) == 0 &&
                authmode_is_usable(records[i].authmode) &&
                records[i].rssi > best_rssi) {
                *authmode = records[i].authmode;
                best_rssi = records[i].rssi;
                found = true;
            }
        }
        if (!found) {
            err = ESP_ERR_NOT_FOUND;
        }
    }
    free(records);
    esp_wifi_clear_ap_list();
    return err;
}

static esp_err_t save_credentials_job(void *context)
{
    const radio_wifi_credentials_t *credentials = context;
    if (credentials == NULL ||
        (credentials->authmode_stored &&
         !authmode_is_usable((wifi_auth_mode_t)credentials->authmode))) {
        return ESP_ERR_INVALID_ARG;
    }
    return radio_wifi_credentials_store_save_job(context);
}

static esp_err_t save_credentials(const char *ssid, const char *password,
                                  bool authmode_detected,
                                  wifi_auth_mode_t authmode)
{
    radio_wifi_credentials_t write = {
        .authmode_stored = authmode_detected,
        .authmode = (uint8_t)authmode,
    };
    snprintf(write.ssid, sizeof(write.ssid), "%s", ssid);
    snprintf(write.password, sizeof(write.password), "%s", password);
    const esp_err_t err = radio_nvs_worker_run_sync(save_credentials_job,
                                                    &write);
    secure_clear(write.password, sizeof(write.password));
    return err;
}

static esp_err_t migrate_wifi_snapshot(const wifi_config_t *config)
{
    if (config == NULL || s_ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    radio_wifi_credentials_t write = {
        .authmode_stored = s_authmode_stored,
        .authmode = (uint8_t)s_authmode,
    };
    memcpy(write.ssid, s_ssid, sizeof(write.ssid));
    memcpy(write.password, config->sta.password,
           sizeof(config->sta.password));
    const esp_err_t err = radio_nvs_worker_run_sync(save_credentials_job,
                                                    &write);
    secure_clear(&write, sizeof(write));
    return err;
}

static void station_connect_if_idle(void)
{
    char ssid[sizeof(s_ssid)];
    wifi_auth_mode_t authmode;
    state_lock();
    if (s_ssid[0] == '\0' || s_station_reconnect_paused || s_connected ||
        s_station_connecting) {
        state_unlock();
        return;
    }

    s_station_connecting = true;
    snprintf(ssid, sizeof(ssid), "%s", s_ssid);
    authmode = s_authmode;
    state_unlock();

    const esp_err_t connect_err = esp_wifi_connect();
    state_lock();
    s_last_connect_error = connect_err;
    if (connect_err != ESP_OK) {
        s_station_connecting = false;
    }
    state_unlock();
    if (connect_err != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo iniciar la conexion Wi-Fi: %s",
                 esp_err_to_name(connect_err));
        schedule_station_retry();
    } else {
        ESP_LOGI(TAG, "Intentando conectar a %s con %s", ssid,
                 wifi_auth_label(authmode));
    }
}

/* Rolls the active candidate back to its previous configuration. When
   generation is non-zero the rollback only applies while that exact candidate
   is still active; this lets a slow caller (for example the commit task after
   an NVS write) avoid discarding a candidate that superseded it in the
   meantime. Pass 0 to roll back whatever candidate is active. */
static void rollback_wifi_candidate(esp_err_t reason, uint32_t generation)
{
    wifi_config_t previous_config = {0};
    bool has_previous_ssid = false;
    state_lock();
    if (!s_wifi_candidate.active ||
        (generation != 0U && s_wifi_candidate.generation != generation)) {
        state_unlock();
        return;
    }
    previous_config = s_wifi_candidate.previous_config;
    has_previous_ssid = s_ssid[0] != '\0';
    secure_clear(&s_wifi_candidate, sizeof(s_wifi_candidate));
    s_station_reconnect_paused = true;
    s_connected = false;
    status_generation_advance_locked();
    s_station_connecting = false;
    s_portal_credentials_submitted = false;
    s_station_retry_failures = 0;
    s_last_connect_error = reason;
    state_unlock();

    (void)esp_wifi_disconnect();
    const esp_err_t restore_err = esp_wifi_set_config(WIFI_IF_STA,
                                                       &previous_config);
    secure_clear(&previous_config, sizeof(previous_config));

    state_lock();
    s_station_reconnect_paused = false;
    if (restore_err != ESP_OK) {
        s_last_connect_error = restore_err;
    }
    state_unlock();
    if (restore_err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo restaurar la red confirmada: %s",
                 esp_err_to_name(restore_err));
    } else if (has_previous_ssid) {
        station_connect_if_idle();
    }
    notify_status();
}

static void candidate_timeout_task(void *arg)
{
    const uint32_t generation = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(WIFI_CANDIDATE_TIMEOUT_MS));
    state_lock();
    const bool timed_out = s_wifi_candidate.active &&
                           s_wifi_candidate.generation == generation &&
                           s_wifi_candidate.attempt_started &&
                           !s_wifi_candidate.commit_pending;
    state_unlock();
    if (timed_out) rollback_wifi_candidate(ESP_ERR_TIMEOUT, 0);
    vTaskDelete(NULL);
}

static void begin_candidate_connection(void)
{
    bool should_connect = false;
    uint32_t generation = 0;
    char ssid[sizeof(s_ssid)] = {0};
    state_lock();
    if (s_wifi_candidate.active && s_wifi_candidate.configuration_ready &&
        s_wifi_candidate.link_down && !s_wifi_candidate.attempt_started) {
        s_wifi_candidate.attempt_started = true;
        generation = s_wifi_candidate.generation;
        s_station_connecting = true;
        s_last_connect_error = ESP_OK;
        snprintf(ssid, sizeof(ssid), "%s", s_wifi_candidate.candidate_ssid);
        should_connect = true;
    }
    state_unlock();
    if (!should_connect) return;

    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        rollback_wifi_candidate(err, 0);
        return;
    }
    if (xTaskCreate(candidate_timeout_task, "wifi_candidate_timeout", 3072,
                    (void *)(uintptr_t)generation, 3, NULL) != pdPASS) {
        rollback_wifi_candidate(ESP_ERR_NO_MEM, 0);
        return;
    }
    ESP_LOGI(TAG, "Probando la red candidata %s", ssid);
    notify_status();
}

static void station_retry_task(void *arg)
{
    const uint32_t delay_ms = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    state_lock();
    s_station_retry_scheduled = false;
    state_unlock();
    station_connect_if_idle();
    vTaskDelete(NULL);
}

static void schedule_station_retry(void)
{
    state_lock();
    if (s_ssid[0] == '\0' || s_station_reconnect_paused || s_connected ||
        s_station_retry_scheduled) {
        state_unlock();
        return;
    }
    if (s_station_retry_failures < 6) ++s_station_retry_failures;
    const uint32_t delay_ms = s_station_retry_failures >= 6
                                  ? 60000U
                                  : (1000U << s_station_retry_failures);
    s_station_retry_scheduled = true;
    state_unlock();
    if (xTaskCreate(station_retry_task, "wifi_retry", 3072,
                    (void *)(uintptr_t)delay_ms, 3, NULL) != pdPASS) {
        state_lock();
        s_station_retry_scheduled = false;
        state_unlock();
        ESP_LOGE(TAG, "No se pudo programar el reintento Wi-Fi");
    } else {
        ESP_LOGI(TAG, "Wi-Fi: nuevo intento en %lu s", (unsigned long)(delay_ms / 1000));
    }
}

static void station_detect_and_connect_task(void *arg)
{
    (void)arg;
    char ssid[sizeof(s_ssid)];
    state_lock();
    snprintf(ssid, sizeof(ssid), "%s", s_ssid);
    state_unlock();

    wifi_auth_mode_t detected = WIFI_AUTH_OPEN;
    const esp_err_t detect_err = detect_authmode(ssid, &detected);
    if (detect_err == ESP_OK) {
        wifi_config_t config = {0};
        esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &config);
        if (err == ESP_OK) {
            config.sta.threshold.authmode = detected;
            config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
            config.sta.pmf_cfg.capable = true;
            config.sta.pmf_cfg.required = false;
            err = esp_wifi_set_config(WIFI_IF_STA, &config);
        }
        if (err == ESP_OK) {
            state_lock();
            s_authmode = detected;
            s_authmode_stored = true;
            state_unlock();
            char password[65] = {0};
            memcpy(password, config.sta.password,
                   sizeof(config.sta.password));
            err = save_credentials(ssid, password, true, detected);
            secure_clear(password, sizeof(password));
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Se detecto %s pero no se pudo guardar: %s",
                     wifi_auth_label(detected), esp_err_to_name(err));
        }
        secure_clear(&config, sizeof(config));
    } else {
        ESP_LOGW(TAG, "No se pudo detectar seguridad de %s; se usara el modo compatible por defecto",
                 ssid);
    }

    state_lock();
    const bool authmode_stored = s_authmode_stored;
    state_unlock();
    if (!authmode_stored) {
        wifi_config_t config = {0};
        esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &config);
        if (err == ESP_OK) {
            config.sta.threshold.authmode =
                config.sta.password[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
            config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
            config.sta.pmf_cfg.capable = true;
            config.sta.pmf_cfg.required = false;
            err = esp_wifi_set_config(WIFI_IF_STA, &config);
        }
        if (err == ESP_OK) {
            state_lock();
            s_authmode = config.sta.threshold.authmode;
            state_unlock();
        } else {
            ESP_LOGW(TAG, "No se pudo configurar seguridad Wi-Fi predeterminada: %s",
                     esp_err_to_name(err));
        }
        secure_clear(&config, sizeof(config));
    }
    station_connect_if_idle();
    vTaskDelete(NULL);
}

static bool station_search_job_cancelled(size_t slot, uint32_t generation)
{
    bool cancelled = true;
    state_lock();
    if (slot < STATION_SEARCH_JOB_COUNT) {
        const station_search_job_t *job = &s_station_search_jobs[slot];
        cancelled = !job->used || job->generation != generation ||
                    job->cancel_requested ||
                    job->state == STATION_SEARCH_CANCELLED;
    }
    state_unlock();
    return cancelled;
}

static esp_err_t station_response_event(esp_http_client_event_t *event)
{
    station_response_t *response = event->user_data;
    if (station_search_job_cancelled(response->slot, response->generation)) {
        response->cancelled = true;
        return ESP_FAIL;
    }
    if (event->event_id == HTTP_EVENT_ON_HEADERS_COMPLETE) {
        const int status_code = esp_http_client_get_status_code(event->client);
        response->accept_body = status_code >= 200 && status_code < 300;
        return ESP_OK;
    }
    /* Ignore intermediate redirect bodies and error pages. */
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0 ||
        !response->accept_body) return ESP_OK;
    const size_t incoming = (size_t)event->data_len;
    if (response->length >= STATION_RESPONSE_MAX ||
        incoming > STATION_RESPONSE_MAX - response->length - 1) {
        response->overflow = true;
        return ESP_FAIL;
    }
    memcpy(response->buffer + response->length, event->data, incoming);
    response->length += incoming;
    response->buffer[response->length] = '\0';
    return ESP_OK;
}

static void query_encode(const char *input, char *output, size_t output_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)input; *p != '\0'; ++p) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
            *p == '.' || *p == '~') {
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

static const char *station_search_endpoint(void)
{
#ifdef CONFIG_ONDA_STATION_SEARCH_TEST_ENDPOINT
    return CONFIG_ONDA_STATION_SEARCH_TEST_ENDPOINT_URL;
#else
    return "https://de1.api.radio-browser.info/json/stations/search";
#endif
}

static bool station_search_is_terminal(station_search_state_t state)
{
    return state == STATION_SEARCH_READY || state == STATION_SEARCH_FAILED ||
           state == STATION_SEARCH_CANCELLED;
}

static void station_search_expire_locked(TickType_t now)
{
    const TickType_t ttl = pdMS_TO_TICKS(STATION_SEARCH_RESULT_TTL_MS);
    for (size_t i = 0; i < STATION_SEARCH_JOB_COUNT; ++i) {
        station_search_job_t *job = &s_station_search_jobs[i];
        if (job->used && station_search_is_terminal(job->state) &&
            job->readers == 0U &&
            (TickType_t)(now - job->last_activity) >= ttl) {
            free(job->response_body);
            memset(job, 0, sizeof(*job));
        }
    }
}

static const char *station_search_state_name(station_search_state_t state)
{
    switch (state) {
    case STATION_SEARCH_QUEUED: return "queued";
    case STATION_SEARCH_RUNNING: return "running";
    case STATION_SEARCH_READY: return "ready";
    case STATION_SEARCH_FAILED: return "failed";
    case STATION_SEARCH_CANCELLED: return "cancelled";
    case STATION_SEARCH_FREE:
    default: return "unknown";
    }
}

static void station_search_worker_task(void *arg)
{
    (void)arg;
    radio_task_supervisor_register(RADIO_SUPERVISED_TASK_STATION_SEARCH);
    station_search_queue_item_t item;

    for (;;) {
        if (xQueueReceive(s_station_search_queue, &item,
                          pdMS_TO_TICKS(1000)) != pdTRUE) {
            /* Reclaim expired results even when the portal stops polling, so an
               abandoned search does not hold its response body indefinitely. */
            state_lock();
            station_search_expire_locked(xTaskGetTickCount());
            state_unlock();
            radio_task_supervisor_progress(
                RADIO_SUPERVISED_TASK_STATION_SEARCH);
            continue;
        }

        char filter[sizeof(((station_search_job_t *)0)->filter)] = {0};
        char query[sizeof(((station_search_job_t *)0)->query)] = {0};
        char codec[sizeof(((station_search_job_t *)0)->codec)] = {0};
        uint16_t offset = 0;
        bool should_run = false;

        state_lock();
        if (item.slot < STATION_SEARCH_JOB_COUNT) {
            station_search_job_t *job = &s_station_search_jobs[item.slot];
            if (job->used && job->generation == item.generation &&
                job->state == STATION_SEARCH_QUEUED) {
                job->last_activity = xTaskGetTickCount();
                if (job->cancel_requested) {
                    job->state = STATION_SEARCH_CANCELLED;
                } else {
                    job->state = STATION_SEARCH_RUNNING;
                    snprintf(filter, sizeof(filter), "%s", job->filter);
                    snprintf(query, sizeof(query), "%s", job->query);
                    snprintf(codec, sizeof(codec), "%s", job->codec);
                    offset = job->offset;
                    should_run = true;
                }
            }
        }
        state_unlock();
        if (!should_run) continue;

        char *body = RADIO_TEST_HEAP_CAPS_MALLOC(
            STATION_RESPONSE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (body == NULL) body = malloc(STATION_RESPONSE_MAX);
        if (body == NULL) {
            state_lock();
            station_search_job_t *job = &s_station_search_jobs[item.slot];
            if (job->used && job->generation == item.generation) {
                job->state = job->cancel_requested ? STATION_SEARCH_CANCELLED
                                                   : STATION_SEARCH_FAILED;
                job->error = ESP_ERR_NO_MEM;
                job->last_activity = xTaskGetTickCount();
            }
            state_unlock();
            continue;
        }
        body[0] = '\0';

        const char *parameter = NULL;
        if (strcmp(filter, "name") == 0) parameter = "name";
        else if (strcmp(filter, "country") == 0) parameter = "country";
        else if (strcmp(filter, "tag") == 0) parameter = "tag";
        else if (strcmp(filter, "language") == 0) parameter = "language";

        char encoded[STATION_QUERY_MAX * 3] = {0};
        char url[512] = {0};
        int url_length = -1;
        query_encode(query, encoded, sizeof(encoded));
        if (parameter != NULL) {
            url_length = snprintf(
                url, sizeof(url),
                "%s?%s=%s&hidebroken=true&codec=%s&order=clickcount&reverse=true&offset=%u&limit=%d",
                station_search_endpoint(), parameter, encoded, codec,
                (unsigned int)offset, STATION_SEARCH_PAGE_SIZE);
        }

        station_response_t response = {
            .buffer = body,
            .slot = item.slot,
            .generation = item.generation,
        };
        const esp_http_client_config_t config = {
            .url = url,
            .event_handler = station_response_event,
            .user_data = &response,
            .timeout_ms = STATION_SEARCH_TIMEOUT_MS,
            .buffer_size = 4096,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .is_async = true,
        };
        esp_err_t err = parameter != NULL && url_length >= 0 &&
                                 (size_t)url_length < sizeof(url)
                             ? ESP_OK : ESP_ERR_INVALID_ARG;
        int status_code = 0;
        esp_http_client_handle_t client = NULL;
        const int64_t request_started_us = esp_timer_get_time();
        if (err == ESP_OK) {
            client = esp_http_client_init(&config);
            if (client == NULL) {
                err = ESP_ERR_NO_MEM;
            } else {
                (void)esp_http_client_set_header(client, "User-Agent",
                                                 "OndaPortatil/0.2 ESP-IDF");
                (void)esp_http_client_set_header(client, "Accept",
                                                 "application/json");
                const TickType_t started = xTaskGetTickCount();
                err = RADIO_TEST_NETWORK_GATE();
                if (err == ESP_OK) err = esp_http_client_perform(client);
                radio_task_supervisor_progress(
                    RADIO_SUPERVISED_TASK_STATION_SEARCH);
                while (err == ESP_ERR_HTTP_EAGAIN) {
                    if (station_search_job_cancelled(item.slot,
                                                     item.generation)) {
                        response.cancelled = true;
                        err = ESP_ERR_INVALID_STATE;
                        break;
                    }
                    if ((TickType_t)(xTaskGetTickCount() - started) >=
                        pdMS_TO_TICKS(STATION_SEARCH_TIMEOUT_MS)) {
                        err = ESP_ERR_TIMEOUT;
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(STATION_SEARCH_POLL_MS));
                    err = RADIO_TEST_NETWORK_GATE();
                    if (err == ESP_OK) err = esp_http_client_perform(client);
                    radio_task_supervisor_progress(
                        RADIO_SUPERVISED_TASK_STATION_SEARCH);
                }
                status_code = esp_http_client_get_status_code(client);
                esp_http_client_cleanup(client);
            }
        }
        ESP_LOGI(TAG,
                 "station search timing elapsed_us=%lld result=%s http=%d bytes=%u cancelled=%u",
                 (long long)(esp_timer_get_time() - request_started_us),
                 esp_err_to_name(err), status_code, (unsigned)response.length,
                 (unsigned)response.cancelled);

        const bool cancelled = response.cancelled ||
                               station_search_job_cancelled(item.slot,
                                                            item.generation);
        const bool successful = !cancelled && err == ESP_OK &&
                                 !response.overflow &&
                                 status_code >= 200 && status_code < 300 &&
                                 response.length > 0U;
        state_lock();
        station_search_job_t *job = &s_station_search_jobs[item.slot];
        if (job->used && job->generation == item.generation) {
            job->last_activity = xTaskGetTickCount();
            if (cancelled || job->cancel_requested) {
                job->state = STATION_SEARCH_CANCELLED;
                job->error = ESP_ERR_INVALID_STATE;
            } else if (successful) {
                job->response_body = body;
                job->response_length = response.length;
                job->state = STATION_SEARCH_READY;
                job->error = ESP_OK;
                body = NULL;
            } else {
                job->state = STATION_SEARCH_FAILED;
                job->error = response.overflow ? ESP_ERR_INVALID_SIZE
                                               : (err == ESP_OK ? ESP_FAIL : err);
            }
        }
        state_unlock();
        free(body);
        radio_task_supervisor_progress(
            RADIO_SUPERVISED_TASK_STATION_SEARCH);
        if (!successful && !cancelled) {
            ESP_LOGW(TAG, "Radio Browser search failed: %s",
                     esp_err_to_name(err == ESP_OK ? ESP_FAIL : err));
        }
    }
}

static esp_err_t ensure_station_search_worker(void)
{
    if (s_station_search_queue != NULL && s_station_search_worker != NULL) {
        return ESP_OK;
    }
    QueueHandle_t queue = xQueueCreate(STATION_SEARCH_QUEUE_LENGTH,
                                       sizeof(station_search_queue_item_t));
    if (queue == NULL) return ESP_ERR_NO_MEM;
    s_station_search_queue = queue;
    TaskHandle_t worker = NULL;
    if (xTaskCreate(station_search_worker_task, "station_search", 8192,
                    NULL, 4, &worker) != pdPASS) {
        s_station_search_queue = NULL;
        vQueueDelete(queue);
        return ESP_ERR_NO_MEM;
    }
    s_station_search_worker = worker;
    return ESP_OK;
}

static esp_err_t station_manager_get_handler(httpd_req_t *request)
{
    if (authorize_portal_request(request, false, NULL, 0) != ESP_OK) {
        return ESP_FAIL;
    }
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send_chunk(request, s_station_manager_page_start,
                                          HTTPD_RESP_USE_STRLEN);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(request, s_station_manager_page_script,
                                    HTTPD_RESP_USE_STRLEN);
    }
    if (err == ESP_OK) err = httpd_resp_send_chunk(request, NULL, 0);
    return err;
}

static esp_err_t station_search_send_job_response(httpd_req_t *request,
                                                 const char *id,
                                                 const char *owner_session)
{
    station_search_state_t state = STATION_SEARCH_FREE;
    bool cancel_requested = false;
    char *body = NULL;
    size_t body_length = 0;
    size_t job_slot = STATION_SEARCH_JOB_COUNT;
    uint32_t job_generation = 0;
    esp_err_t job_error = ESP_OK;
    bool found = false;

    state_lock();
    station_search_expire_locked(xTaskGetTickCount());
    for (size_t i = 0; i < STATION_SEARCH_JOB_COUNT; ++i) {
        station_search_job_t *job = &s_station_search_jobs[i];
        if (job->used &&
            constant_time_string_equal(job->id, id,
                                        PORTAL_SESSION_TOKEN_LENGTH)) {
            if (!constant_time_string_equal(job->owner_session, owner_session,
                                             PORTAL_SESSION_TOKEN_LENGTH)) {
                break;
            }
            job->last_activity = xTaskGetTickCount();
            state = job->state;
            cancel_requested = job->cancel_requested;
            job_error = job->error;
            body = job->response_body;
            body_length = job->response_length;
            job_slot = i;
            job_generation = job->generation;
            if (state == STATION_SEARCH_READY && body != NULL &&
                job->readers < UINT8_MAX) {
                ++job->readers;
            }
            found = true;
            break;
        }
    }
    state_unlock();

    if (!found) {
        return reject_portal_request(request, "404 Not Found",
                                     "Búsqueda inexistente o vencida");
    }

    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    char response[160];
    if (state == STATION_SEARCH_READY && body != NULL) {
        snprintf(response, sizeof(response),
                 "{\"id\":\"%s\",\"state\":\"ready\",\"results\":",
                 id);
        esp_err_t err = httpd_resp_send_chunk(request, response,
                                              HTTPD_RESP_USE_STRLEN);
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(request, body, body_length);
        }
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(request, "}", HTTPD_RESP_USE_STRLEN);
        }
        if (err == ESP_OK) err = httpd_resp_send_chunk(request, NULL, 0);
        state_lock();
        station_search_job_t *job = &s_station_search_jobs[job_slot];
        if (job->used && job->generation == job_generation &&
            job->readers > 0U) {
            --job->readers;
        }
        state_unlock();
        return err;
    }
    if (state == STATION_SEARCH_FAILED) {
        const char *error_name = job_error == ESP_ERR_NO_MEM ? "memory" :
                                 (job_error == ESP_ERR_TIMEOUT ||
                                  job_error == ESP_ERR_HTTP_READ_TIMEOUT)
                                     ? "timeout" :
                                 "upstream";
        snprintf(response, sizeof(response),
                 "{\"id\":\"%s\",\"state\":\"failed\",\"error\":\"%s\"}",
                 id, error_name);
    } else {
        const char *state_name = cancel_requested ? "cancelling" :
                                 station_search_state_name(state);
        snprintf(response, sizeof(response),
                 "{\"id\":\"%s\",\"state\":\"%s\"}", id,
                 state_name);
    }
    return httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t station_search_handler(httpd_req_t *request)
{
    if (authorize_portal_request(request, false, NULL, 0) != ESP_OK) {
        return ESP_FAIL;
    }
    char owner_session[PORTAL_SESSION_TOKEN_LENGTH + 1U] = {0};
    if (!get_session_id_from_cookie(request, owner_session)) {
        secure_clear(owner_session, sizeof(owner_session));
        return reject_portal_request(request, "401 Unauthorized",
                                     "Autenticación requerida");
    }

    char query_string[STATION_QUERY_MAX * 3 + 64] = {0};
    char job_id[PORTAL_SESSION_TOKEN_LENGTH + 1U] = {0};
    if (httpd_req_get_url_query_str(request, query_string,
                                    sizeof(query_string)) != ESP_OK) {
        secure_clear(owner_session, sizeof(owner_session));
        return reject_portal_request(request, "400 Bad Request",
                                     "Faltan parámetros de búsqueda");
    }
    if (httpd_query_key_value(query_string, "id", job_id,
                              sizeof(job_id)) == ESP_OK) {
        if (!is_lowercase_hex_token(job_id, strlen(job_id))) {
            secure_clear(owner_session, sizeof(owner_session));
            return reject_portal_request(request, "400 Bad Request",
                                         "Identificador inválido");
        }
        const esp_err_t poll_err = station_search_send_job_response(
            request, job_id, owner_session);
        secure_clear(owner_session, sizeof(owner_session));
        return poll_err;
    }

    char filter_encoded[24] = {0};
    char value_encoded[STATION_QUERY_MAX * 3] = {0};
    char offset_text[8] = {0};
    char filter[24] = {0};
    char value[STATION_QUERY_MAX] = {0};
    char codec[8] = "MP3";
    if (httpd_query_key_value(query_string, "filter", filter_encoded,
                              sizeof(filter_encoded)) != ESP_OK ||
        httpd_query_key_value(query_string, "q", value_encoded,
                              sizeof(value_encoded)) != ESP_OK) {
        secure_clear(owner_session, sizeof(owner_session));
        return reject_portal_request(request, "400 Bad Request",
                                     "Falta el filtro o la búsqueda");
    }
    if (!radio_url_form_decode(filter, sizeof(filter), filter_encoded,
                               strlen(filter_encoded)) ||
        !radio_url_form_decode(value, sizeof(value), value_encoded,
                               strlen(value_encoded))) {
        secure_clear(owner_session, sizeof(owner_session));
        return reject_portal_request(request, "400 Bad Request",
                                     "Filtro o búsqueda malformados");
    }
    if (httpd_query_key_value(query_string, "codec", codec,
                              sizeof(codec)) == ESP_OK &&
        strcmp(codec, "MP3") != 0 && strcmp(codec, "AAC") != 0) {
        secure_clear(owner_session, sizeof(owner_session));
        return reject_portal_request(request, "400 Bad Request",
                                     "Formato inválido; elegí MP3 o AAC");
    }
    const bool valid_filter = strcmp(filter, "name") == 0 ||
                              strcmp(filter, "country") == 0 ||
                              strcmp(filter, "tag") == 0 ||
                              strcmp(filter, "language") == 0;
    if (!valid_filter || value[0] == '\0') {
        secure_clear(owner_session, sizeof(owner_session));
        return reject_portal_request(request, "400 Bad Request",
                                     "Filtro o búsqueda inválidos");
    }

    unsigned long offset = 0;
    if (httpd_query_key_value(query_string, "offset", offset_text,
                              sizeof(offset_text)) == ESP_OK) {
        char *end = NULL;
        offset = strtoul(offset_text, &end, 10);
        if (offset_text[0] == '\0' || *end != '\0' ||
            offset > STATION_SEARCH_MAX_OFFSET ||
            offset % STATION_SEARCH_PAGE_SIZE != 0) {
            secure_clear(owner_session, sizeof(owner_session));
            return reject_portal_request(request, "400 Bad Request",
                                         "Página inválida");
        }
    }

    char generated_id[PORTAL_SESSION_TOKEN_LENGTH + 1U] = {0};
    random_token_hex(generated_id);
    station_search_queue_item_t queue_item = {0};
    bool queued = false;
    state_lock();
    const TickType_t now = xTaskGetTickCount();
    station_search_expire_locked(now);
    size_t selected_slot = STATION_SEARCH_JOB_COUNT;
    size_t same_session_slot = STATION_SEARCH_JOB_COUNT;
    size_t fallback_slot = STATION_SEARCH_JOB_COUNT;
    TickType_t oldest_same_session_age = 0;
    TickType_t oldest_fallback_age = 0;
    for (size_t i = 0; i < STATION_SEARCH_JOB_COUNT; ++i) {
        station_search_job_t *candidate = &s_station_search_jobs[i];
        if (!candidate->used) {
            selected_slot = i;
            break;
        }
        if (candidate->readers == 0U &&
            station_search_is_terminal(candidate->state)) {
            const TickType_t age = (TickType_t)(now - candidate->last_activity);
            if (constant_time_string_equal(candidate->owner_session,
                                           owner_session,
                                           PORTAL_SESSION_TOKEN_LENGTH)) {
                if (same_session_slot == STATION_SEARCH_JOB_COUNT ||
                    age > oldest_same_session_age) {
                    same_session_slot = i;
                    oldest_same_session_age = age;
                }
            } else if (fallback_slot == STATION_SEARCH_JOB_COUNT ||
                       age > oldest_fallback_age) {
                fallback_slot = i;
                oldest_fallback_age = age;
            }
        }
    }
    if (selected_slot == STATION_SEARCH_JOB_COUNT) {
        selected_slot = same_session_slot != STATION_SEARCH_JOB_COUNT
                            ? same_session_slot : fallback_slot;
    }
    if (selected_slot < STATION_SEARCH_JOB_COUNT) {
        station_search_job_t *job = &s_station_search_jobs[selected_slot];
        free(job->response_body);
        memset(job, 0, sizeof(*job));
        job->used = true;
        if (++s_station_search_generation == 0U) {
            ++s_station_search_generation;
        }
        job->generation = s_station_search_generation;
        job->state = STATION_SEARCH_QUEUED;
        snprintf(job->id, sizeof(job->id), "%s", generated_id);
        snprintf(job->owner_session, sizeof(job->owner_session), "%s",
                 owner_session);
        snprintf(job->filter, sizeof(job->filter), "%s", filter);
        snprintf(job->query, sizeof(job->query), "%s", value);
        snprintf(job->codec, sizeof(job->codec), "%s", codec);
        job->offset = (uint16_t)offset;
        job->last_activity = now;
        queue_item.slot = selected_slot;
        queue_item.generation = job->generation;
        queued = s_station_search_queue != NULL &&
                  RADIO_TEST_QUEUE_SEND(s_station_search_queue, &queue_item,
                                        0) == pdTRUE;
        if (!queued) memset(job, 0, sizeof(*job));
    }
    state_unlock();
    memset(owner_session, 0, sizeof(owner_session));

    if (!queued) {
        return reject_portal_request(request, "503 Service Unavailable",
                                     "La búsqueda está ocupada; intentá más tarde");
    }
    char response[96];
    snprintf(response, sizeof(response),
             "{\"id\":\"%s\",\"state\":\"queued\"}", generated_id);
    httpd_resp_set_status(request, "202 Accepted");
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
}

static bool add_station_json(cJSON *object, const radio_station_t *station)
{
    return cJSON_AddStringToObject(object, "name", station->name) != NULL &&
           cJSON_AddStringToObject(object, "country", station->country) != NULL &&
           cJSON_AddStringToObject(object, "codec", station->codec) != NULL &&
           cJSON_AddStringToObject(object, "uuid", station->uuid) != NULL &&
           cJSON_AddStringToObject(object, "url", station->url) != NULL;
}

static esp_err_t station_favorites_get_handler(httpd_req_t *request)
{
    if (authorize_portal_request(request, false, NULL, 0) != ESP_OK) {
        return ESP_FAIL;
    }
    radio_station_t *stations = calloc(RADIO_FAVORITES_MAX, sizeof(*stations));
    if (stations == NULL) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Sin memoria");
        return ESP_ERR_NO_MEM;
    }
    const size_t count = radio_station_store_favorites(stations, RADIO_FAVORITES_MAX);
    cJSON *array = cJSON_CreateArray();
    if (array == NULL) {
        free(stations);
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Sin memoria");
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < count; ++i) {
        cJSON *item = cJSON_CreateObject();
        if (item == NULL || !add_station_json(item, &stations[i])) {
            cJSON_Delete(item);
            cJSON_Delete(array);
            free(stations);
            httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Sin memoria");
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(array, item);
    }
    free(stations);
    char *json = cJSON_PrintUnformatted(array);
    cJSON_Delete(array);
    if (json == NULL) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Sin memoria");
        return ESP_ERR_NO_MEM;
    }
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_send(request, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    return err;
}

static esp_err_t read_station_json(httpd_req_t *request, char *body,
                                    size_t capacity, size_t *body_length)
{
    if (body_length != NULL) *body_length = 0U;
    if (request->content_len <= 0 || (size_t)request->content_len >= capacity) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Solicitud demasiado larga");
        return ESP_FAIL;
    }
    size_t received = 0;
    while (received < (size_t)request->content_len) {
        const int count = httpd_req_recv(request, body + received,
                                         request->content_len - received);
        if (count <= 0) {
            httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Solicitud incompleta");
            return ESP_FAIL;
        }
        received += (size_t)count;
    }
    body[received] = '\0';
    if (body_length != NULL) *body_length = received;
    return ESP_OK;
}

static esp_err_t station_search_cancel_handler(httpd_req_t *request)
{
    if (authorize_portal_request(request, true, NULL, 0) != ESP_OK) {
        return ESP_FAIL;
    }
    if (!request_has_json_content_type(request)) {
        return reject_portal_request(request, "400 Bad Request",
                                     "Se requiere una solicitud JSON");
    }

    char owner_session[PORTAL_SESSION_TOKEN_LENGTH + 1U] = {0};
    char body[96] = {0};
    size_t body_length = 0U;
    if (!get_session_id_from_cookie(request, owner_session) ||
        read_station_json(request, body, sizeof(body), &body_length) != ESP_OK) {
        secure_clear(owner_session, sizeof(owner_session));
        secure_clear(body, sizeof(body));
        return ESP_FAIL;
    }
    cJSON *root = radio_json_parse_exact(body, body_length);
    secure_clear(body, sizeof(body));
    const cJSON *id_item = root != NULL
                               ? cJSON_GetObjectItemCaseSensitive(root, "id")
                               : NULL;
    char id[PORTAL_SESSION_TOKEN_LENGTH + 1U] = {0};
    const bool valid_id = cJSON_IsString(id_item) &&
                          is_lowercase_hex_token(id_item->valuestring,
                                                strlen(id_item->valuestring));
    if (valid_id) snprintf(id, sizeof(id), "%s", id_item->valuestring);
    cJSON_Delete(root);
    if (!valid_id) {
        secure_clear(owner_session, sizeof(owner_session));
        return reject_portal_request(request, "400 Bad Request",
                                     "Identificador de búsqueda inválido");
    }

    bool found = false;
    bool cancel_accepted = false;
    state_lock();
    station_search_expire_locked(xTaskGetTickCount());
    for (size_t i = 0; i < STATION_SEARCH_JOB_COUNT; ++i) {
        station_search_job_t *job = &s_station_search_jobs[i];
        if (job->used &&
            constant_time_string_equal(job->id, id,
                                        PORTAL_SESSION_TOKEN_LENGTH)) {
            if (!constant_time_string_equal(job->owner_session, owner_session,
                                             PORTAL_SESSION_TOKEN_LENGTH)) {
                break;
            }
            found = true;
            if (job->state == STATION_SEARCH_QUEUED) {
                job->cancel_requested = true;
                job->state = STATION_SEARCH_CANCELLED;
                job->error = ESP_ERR_INVALID_STATE;
                job->last_activity = xTaskGetTickCount();
                cancel_accepted = true;
            } else if (job->state == STATION_SEARCH_RUNNING) {
                job->cancel_requested = true;
                job->last_activity = xTaskGetTickCount();
                cancel_accepted = true;
            } else if (job->state == STATION_SEARCH_CANCELLED) {
                cancel_accepted = true;
            }
            break;
        }
    }
    state_unlock();
    secure_clear(owner_session, sizeof(owner_session));
    if (!found) {
        return reject_portal_request(request, "404 Not Found",
                                     "Búsqueda inexistente o vencida");
    }
    if (!cancel_accepted) {
        return reject_portal_request(request, "409 Conflict",
                                     "La búsqueda ya terminó");
    }
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, "{\"cancel_requested\":true}",
                           HTTPD_RESP_USE_STRLEN);
}

static esp_err_t station_favorite_post_handler(httpd_req_t *request)
{
    if (authorize_portal_request(request, true, NULL, 0) != ESP_OK) {
        return ESP_FAIL;
    }
    char owner_session[PORTAL_SESSION_TOKEN_LENGTH + 1U] = {0};
    if (!get_session_id_from_cookie(request, owner_session)) {
        return reject_portal_request(request, "401 Unauthorized",
                                     "Autenticación requerida");
    }
    char *body = malloc(STATION_POST_MAX + 1);
    if (body == NULL) {
        secure_clear(owner_session, sizeof(owner_session));
        return ESP_ERR_NO_MEM;
    }
    size_t body_length = 0U;
    if (read_station_json(request, body, STATION_POST_MAX + 1,
                          &body_length) != ESP_OK) {
        free(body);
        secure_clear(owner_session, sizeof(owner_session));
        return ESP_FAIL;
    }
    radio_station_t station = {0};
    const esp_err_t decode_err =
        radio_station_json_decode(body, body_length, &station);
    secure_clear(body, STATION_POST_MAX + 1U);
    free(body);
    if (decode_err != ESP_OK) {
        secure_clear(owner_session, sizeof(owner_session));
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Emisora inválida");
        return ESP_FAIL;
    }
    char mutation_id[PORTAL_SESSION_TOKEN_LENGTH + 1U] = {0};
    portal_mutation_context_t *mutation_context = NULL;
    esp_err_t err = portal_mutation_begin(owner_session, mutation_id,
                                          &mutation_context);
    secure_clear(owner_session, sizeof(owner_session));
    if (err != ESP_OK) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        httpd_resp_send(request, "El almacenamiento está ocupado",
                        HTTPD_RESP_USE_STRLEN);
        return err;
    }
    err = radio_station_store_toggle_favorite_async(
        &station, portal_mutation_complete, mutation_context);
    if (err != ESP_OK) {
        portal_mutation_discard(mutation_context);
        secure_clear(mutation_id, sizeof(mutation_id));
        httpd_resp_set_status(request, "503 Service Unavailable");
        httpd_resp_send(request, "El almacenamiento está ocupado",
                        HTTPD_RESP_USE_STRLEN);
        return err;
    }
    char response[80];
    snprintf(response, sizeof(response), "{\"id\":\"%s\",\"state\":\"queued\"}",
             mutation_id);
    secure_clear(mutation_id, sizeof(mutation_id));
    httpd_resp_set_status(request, "202 Accepted");
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t station_reorder_post_handler(httpd_req_t *request)
{
    if (authorize_portal_request(request, true, NULL, 0) != ESP_OK) {
        return ESP_FAIL;
    }
    char owner_session[PORTAL_SESSION_TOKEN_LENGTH + 1U] = {0};
    if (!get_session_id_from_cookie(request, owner_session)) {
        return reject_portal_request(request, "401 Unauthorized",
                                     "Autenticación requerida");
    }
    char *body = malloc(STATION_POST_MAX + 1);
    if (body == NULL) {
        secure_clear(owner_session, sizeof(owner_session));
        return ESP_ERR_NO_MEM;
    }
    size_t body_length = 0U;
    if (read_station_json(request, body, STATION_POST_MAX + 1,
                          &body_length) != ESP_OK) {
        free(body);
        secure_clear(owner_session, sizeof(owner_session));
        return ESP_FAIL;
    }
    cJSON *root = radio_json_parse_exact(body, body_length);
    secure_clear(body, STATION_POST_MAX + 1U);
    free(body);
    const cJSON *uuid_item = root != NULL ? cJSON_GetObjectItemCaseSensitive(root, "uuid") : NULL;
    const cJSON *direction_item = root != NULL ? cJSON_GetObjectItemCaseSensitive(root, "direction") : NULL;
    if (!cJSON_IsString(uuid_item) || !cJSON_IsNumber(direction_item)) {
        cJSON_Delete(root);
        secure_clear(owner_session, sizeof(owner_session));
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Orden inválido");
        return ESP_FAIL;
    }
    char mutation_id[PORTAL_SESSION_TOKEN_LENGTH + 1U] = {0};
    portal_mutation_context_t *mutation_context = NULL;
    esp_err_t err = portal_mutation_begin(owner_session, mutation_id,
                                          &mutation_context);
    secure_clear(owner_session, sizeof(owner_session));
    if (err != ESP_OK) {
        cJSON_Delete(root);
        httpd_resp_set_status(request, "503 Service Unavailable");
        httpd_resp_send(request, "El almacenamiento está ocupado",
                        HTTPD_RESP_USE_STRLEN);
        return err;
    }
    err = radio_station_store_move_favorite_async(
        uuid_item->valuestring, direction_item->valueint,
        portal_mutation_complete, mutation_context);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        portal_mutation_discard(mutation_context);
        secure_clear(mutation_id, sizeof(mutation_id));
        httpd_resp_set_status(request, "503 Service Unavailable");
        httpd_resp_send(request, "El almacenamiento está ocupado",
                        HTTPD_RESP_USE_STRLEN);
        return err;
    }
    char response[80];
    snprintf(response, sizeof(response), "{\"id\":\"%s\",\"state\":\"queued\"}",
             mutation_id);
    secure_clear(mutation_id, sizeof(mutation_id));
    httpd_resp_set_status(request, "202 Accepted");
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t station_mutation_get_handler(httpd_req_t *request)
{
    if (authorize_portal_request(request, false, NULL, 0) != ESP_OK) {
        return ESP_FAIL;
    }
    char owner_session[PORTAL_SESSION_TOKEN_LENGTH + 1U] = {0};
    char query_string[96] = {0};
    char mutation_id[PORTAL_SESSION_TOKEN_LENGTH + 1U] = {0};
    if (!get_session_id_from_cookie(request, owner_session) ||
        httpd_req_get_url_query_str(request, query_string,
                                    sizeof(query_string)) != ESP_OK ||
        httpd_query_key_value(query_string, "id", mutation_id,
                              sizeof(mutation_id)) != ESP_OK ||
        !is_lowercase_hex_token(mutation_id, strlen(mutation_id))) {
        secure_clear(owner_session, sizeof(owner_session));
        secure_clear(mutation_id, sizeof(mutation_id));
        return reject_portal_request(request, "400 Bad Request",
                                     "Identificador de operación inválido");
    }
    const esp_err_t err = portal_mutation_send_status(
        request, mutation_id, owner_session);
    secure_clear(owner_session, sizeof(owner_session));
    secure_clear(mutation_id, sizeof(mutation_id));
    return err;
}

static esp_err_t portal_get_handler(httpd_req_t *request)
{
    if (require_softap_request(request) != ESP_OK) return ESP_FAIL;
    /* The cookie buffer must outlive the response: httpd_resp_set_hdr keeps the
       pointer and only serializes the value inside httpd_resp_send. */
    char cookie[96] = {0};
    const esp_err_t session_err =
        portal_session_open_from_softap(request, cookie, sizeof(cookie));
    if (session_err != ESP_OK) {
        secure_clear(cookie, sizeof(cookie));
        return session_err;
    }
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Pragma", "no-cache");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Set-Cookie", cookie);
    const esp_err_t send_err =
        httpd_resp_send(request, s_portal_page, HTTPD_RESP_USE_STRLEN);
    secure_clear(cookie, sizeof(cookie));
    return send_err;
}

static esp_err_t portal_scan_handler(httpd_req_t *request)
{
    if (authorize_portal_request(request, false, NULL, 0) != ESP_OK) {
        return ESP_FAIL;
    }
    wifi_scan_config_t scan_config = {
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {
            .min = 100,
            .max = 150,
        },
        .show_hidden = false,
    };
    const esp_err_t scan_err = esp_wifi_scan_start(&scan_config, true);
    if (scan_err != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo buscar redes Wi-Fi: %s", esp_err_to_name(scan_err));
        httpd_resp_set_status(request, "503 Service Unavailable");
        httpd_resp_send(request, "No se pudo buscar redes. Intentá de nuevo.",
                        HTTPD_RESP_USE_STRLEN);
        return scan_err;
    }

    uint16_t found_count = 0;
    esp_err_t err = esp_wifi_scan_get_ap_num(&found_count);
    if (err != ESP_OK) {
        esp_wifi_clear_ap_list();
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "No se pudo leer la búsqueda");
        return err;
    }
    if (found_count == 0) {
        esp_wifi_clear_ap_list();
        httpd_resp_set_type(request, "application/json");
        return httpd_resp_send(request, "[]", HTTPD_RESP_USE_STRLEN);
    }

    const uint16_t capacity = found_count > WIFI_SCAN_RESULT_LIMIT
                                  ? WIFI_SCAN_RESULT_LIMIT : found_count;
    wifi_ap_record_t *records = calloc(capacity, sizeof(*records));
    if (records == NULL) {
        esp_wifi_clear_ap_list();
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "No hay memoria para leer las redes");
        return ESP_ERR_NO_MEM;
    }
    uint16_t record_count = capacity;
    err = esp_wifi_scan_get_ap_records(&record_count, records);
    if (err != ESP_OK) {
        free(records);
        esp_wifi_clear_ap_list();
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "No se pudo leer la búsqueda");
        return err;
    }

    cJSON *networks = cJSON_CreateArray();
    if (networks == NULL) {
        free(records);
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "No hay memoria para preparar las redes");
        return ESP_ERR_NO_MEM;
    }

    char seen[WIFI_SCAN_RESULT_LIMIT][33] = {{0}};
    size_t seen_count = 0;
    for (uint16_t i = 0; i < record_count; ++i) {
        if (!authmode_is_usable(records[i].authmode)) {
            continue;
        }
        char ssid[33];
        memcpy(ssid, records[i].ssid, sizeof(records[i].ssid));
        ssid[sizeof(ssid) - 1] = '\0';
        if (ssid[0] == '\0') {
            continue;
        }
        bool duplicate = false;
        for (size_t j = 0; j < seen_count; ++j) {
            if (strcmp(seen[j], ssid) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        memcpy(seen[seen_count++], ssid, strlen(ssid) + 1);

        cJSON *network = cJSON_CreateObject();
        if (network == NULL ||
            !cJSON_AddStringToObject(network, "ssid", ssid) ||
            !cJSON_AddNumberToObject(network, "rssi", records[i].rssi) ||
            !cJSON_AddNumberToObject(network, "authmode", records[i].authmode) ||
            !cJSON_AddStringToObject(network, "security",
                                     wifi_auth_label(records[i].authmode)) ||
            !cJSON_AddItemToArray(networks, network)) {
            cJSON_Delete(network);
            cJSON_Delete(networks);
            free(records);
            httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "No hay memoria para preparar las redes");
            return ESP_ERR_NO_MEM;
        }
    }
    free(records);

    char *json = cJSON_PrintUnformatted(networks);
    cJSON_Delete(networks);
    if (json == NULL) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "No hay memoria para enviar las redes");
        return ESP_ERR_NO_MEM;
    }
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    const esp_err_t send_err = httpd_resp_send(request, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    return send_err;
}

static esp_err_t portal_status_handler(httpd_req_t *request)
{
    if (authorize_portal_request(request, false, NULL, 0) != ESP_OK) {
        return ESP_FAIL;
    }
    char status[256];
    bool connected;
    bool connecting;
    uint8_t disconnect_reason;
    uint8_t disconnect_count;
    esp_err_t connect_error;
    wifi_auth_mode_t authmode;
    state_lock();
    connected = s_connected;
    connecting = s_station_connecting;
    disconnect_reason = s_last_disconnect_reason;
    disconnect_count = s_disconnect_count;
    connect_error = s_last_connect_error;
    authmode = s_authmode;
    state_unlock();
    snprintf(status, sizeof(status),
             "{\"connected\":%s,\"connecting\":%s,\"reason\":%u,"
             "\"disconnects\":%u,\"connect_error\":\"%s\",\"security\":\"%s\"}",
             connected ? "true" : "false", connecting ? "true" : "false",
             (unsigned int)disconnect_reason, (unsigned int)disconnect_count,
             esp_err_to_name(connect_error), wifi_auth_label(authmode));
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    return httpd_resp_send(request, status, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_and_connect(const char *ssid, const char *password,
                                  int authmode, bool from_portal);

static esp_err_t portal_post_handler(httpd_req_t *request)
{
    if (authorize_portal_request(request, true, NULL, 0) != ESP_OK) {
        return ESP_FAIL;
    }
    if (request->content_len <= 0 || request->content_len >= FORM_BODY_MAX) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Formulario inválido");
        return ESP_FAIL;
    }

    char body[FORM_BODY_MAX];
    size_t received = 0;
    while (received < (size_t)request->content_len) {
        const int count = httpd_req_recv(request, body + received,
                                         request->content_len - received);
        if (count <= 0) {
            secure_clear(body, sizeof(body));
            httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "No se recibió el formulario");
            return ESP_FAIL;
        }
        received += (size_t)count;
    }
    body[received] = '\0';

    radio_wifi_form_t form = {0};
    const bool form_valid = radio_wifi_form_parse(body, received, &form);
    secure_clear(body, sizeof(body));
    if (!form_valid) {
        secure_clear(&form, sizeof(form));
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                            "Formulario inválido");
        return ESP_FAIL;
    }

    const esp_err_t err = save_and_connect(form.ssid, form.password,
                                           form.authmode, true);
    secure_clear(&form, sizeof(form));
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    if (err != ESP_OK) {
        static const char error_page[] =
            "<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<meta charset='utf-8'><body style='font:18px system-ui;background:#10141a;color:white;"
            "padding:24px'><h2>No se pudo guardar</h2><p>Revisá el nombre de la red e intentá otra vez.</p>"
            "<a style='color:#ffbf69' href='/'>Volver</a></body>";
        httpd_resp_set_status(request, "400 Bad Request");
        return httpd_resp_send(request, error_page, HTTPD_RESP_USE_STRLEN);
    }

    ESP_LOGI(TAG, "Credenciales recibidas desde el portal");
    return httpd_resp_send(request, s_connecting_page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t start_http_server(void)
{
    if (s_http_server != NULL) {
        if (s_http_server_ready) return ESP_OK;
        /* A previous partial startup could not stop; never reuse its routes. */
        const esp_err_t stop_err = httpd_stop(s_http_server);
        if (stop_err != ESP_OK) {
            ESP_LOGE(TAG, "No se pudo limpiar el servidor incompleto: %s",
                     esp_err_to_name(stop_err));
            return stop_err;
        }
        s_http_server = NULL;
    }
    ESP_RETURN_ON_ERROR(ensure_station_search_worker(), TAG,
                        "No se pudo iniciar la búsqueda de emisoras");

    static const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = portal_get_handler},
        {.uri = "/auth/csrf", .method = HTTP_GET, .handler = portal_csrf_handler},
        {.uri = "/auth/logout", .method = HTTP_POST, .handler = portal_logout_handler},
        {.uri = "/wifi", .method = HTTP_POST, .handler = portal_post_handler},
        {.uri = "/scan", .method = HTTP_GET, .handler = portal_scan_handler},
        {.uri = "/status", .method = HTTP_GET, .handler = portal_status_handler},
        {.uri = "/radios", .method = HTTP_GET, .handler = station_manager_get_handler},
        {.uri = "/stations/search", .method = HTTP_GET, .handler = station_search_handler},
        {.uri = "/stations/favorites", .method = HTTP_GET, .handler = station_favorites_get_handler},
        {.uri = "/stations/favorite", .method = HTTP_POST, .handler = station_favorite_post_handler},
        {.uri = "/stations/reorder", .method = HTTP_POST, .handler = station_reorder_post_handler},
        {.uri = "/stations/mutation", .method = HTTP_GET, .handler = station_mutation_get_handler},
        {.uri = "/stations/search/cancel", .method = HTTP_POST, .handler = station_search_cancel_handler},
    };

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    /* Keep the server capacity in sync with every registered portal route. */
    config.max_uri_handlers = sizeof(handlers) / sizeof(handlers[0]);
    config.stack_size = 8192;
    ESP_RETURN_ON_ERROR(httpd_start(&s_http_server, &config), TAG,
                        "No se pudo iniciar la pagina de configuracion");

    esp_err_t err = ESP_OK;
    for (size_t i = 0; err == ESP_OK && i < sizeof(handlers) / sizeof(handlers[0]);
         ++i) {
        err = httpd_register_uri_handler(s_http_server, &handlers[i]);
    }
    if (err != ESP_OK) {
        const esp_err_t stop_err = httpd_stop(s_http_server);
        if (stop_err == ESP_OK) {
            s_http_server = NULL;
        } else {
            ESP_LOGE(TAG, "Fallo al limpiar servidor incompleto: %s",
                     esp_err_to_name(stop_err));
        }
        return err;
    }
    s_http_server_ready = true;
    return ESP_OK;
}

static esp_err_t recover_station_after_portal_error(esp_err_t error)
{
    state_lock();
    s_station_reconnect_paused = false;
    const bool has_saved_ssid = s_ssid[0] != '\0';
    state_unlock();
    const esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (mode_err != ESP_OK) {
        ESP_LOGE(TAG, "Fallo del portal: %s; no se pudo restaurar STA: %s",
                 esp_err_to_name(error), esp_err_to_name(mode_err));
        return mode_err;
    }
    if (has_saved_ssid) {
        station_connect_if_idle();
    }
    return error;
}

static void portal_shutdown_task(void *arg)
{
    const uint32_t generation = (uint32_t)(uintptr_t)arg;
    bool restore_ap_pending = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PORTAL_SHUTDOWN_DELAY_MS));
        xSemaphoreTake(s_portal_lifecycle_lock, portMAX_DELAY);
        state_lock();
        const bool pending_shutdown = s_portal_active &&
                                      s_portal_credentials_submitted;
        const bool connected = s_connected;
        state_unlock();
        if (!pending_shutdown) {
            xSemaphoreGive(s_portal_lifecycle_lock);
            break;
        }
        if (restore_ap_pending) {
            const esp_err_t restore_err = esp_wifi_set_mode(WIFI_MODE_APSTA);
            if (restore_err != ESP_OK) {
                ESP_LOGW(TAG, "Sigue pendiente restaurar el SoftAP: %s",
                         esp_err_to_name(restore_err));
                xSemaphoreGive(s_portal_lifecycle_lock);
                continue;
            }
            restore_ap_pending = false;
        }
        /* A temporary STA outage must not cancel the pending portal close. */
        if (!connected) {
            xSemaphoreGive(s_portal_lifecycle_lock);
            continue;
        }

        wifi_sta_list_t stations = {0};
        const esp_err_t list_err = esp_wifi_ap_get_sta_list(&stations);
        if (list_err != ESP_OK || stations.num > 0) {
            /* Keep the local portal reachable while a phone is still using it. */
            if (list_err != ESP_OK) {
                ESP_LOGW(TAG, "No se pudo comprobar si quedan clientes del portal: %s",
                         esp_err_to_name(list_err));
            }
            xSemaphoreGive(s_portal_lifecycle_lock);
            continue;
        }

        const esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "No se pudo cerrar el portal: %s", esp_err_to_name(err));
            xSemaphoreGive(s_portal_lifecycle_lock);
            continue;
        }
        if (s_http_server != NULL) {
            const esp_err_t stop_err = httpd_stop(s_http_server);
            if (stop_err != ESP_OK) {
                ESP_LOGW(TAG, "No se pudo detener el servidor del portal: %s",
                         esp_err_to_name(stop_err));
                const esp_err_t restore_err = esp_wifi_set_mode(WIFI_MODE_APSTA);
                if (restore_err != ESP_OK) {
                    ESP_LOGE(TAG, "No se pudo restaurar el SoftAP: %s",
                             esp_err_to_name(restore_err));
                    restore_ap_pending = true;
                }
                xSemaphoreGive(s_portal_lifecycle_lock);
                continue;
            }
            s_http_server = NULL;
            s_http_server_ready = false;
        }
        state_lock();
        s_portal_active = false;
        status_generation_advance_locked();
        s_portal_credentials_submitted = false;
        for (size_t i = 0; i < STATION_SEARCH_JOB_COUNT; ++i) {
            station_search_job_t *job = &s_station_search_jobs[i];
            if (job->used && station_search_is_terminal(job->state) &&
                job->readers == 0U) {
                free(job->response_body);
                secure_clear(job, sizeof(*job));
            }
        }
        if (s_portal_shutdown_generation == generation) {
            s_portal_shutdown_scheduled = false;
        }
        clear_portal_sessions_locked();
        state_unlock();
        xSemaphoreGive(s_portal_lifecycle_lock);
        ESP_LOGI(TAG, "Portal cerrado; no quedan clientes locales conectados");
        notify_status();
        break;
    }

    state_lock();
    /* Only the owning task releases the pending flag, so a task superseded by a
       newer generation cannot cancel that newer task's shutdown. */
    if (s_portal_shutdown_generation == generation) {
        s_portal_shutdown_scheduled = false;
    }
    state_unlock();
    vTaskDelete(NULL);
}

static void candidate_commit_task(void *arg)
{
    (void)arg;
    char ssid[sizeof(s_ssid)] = {0};
    char password[65] = {0};
    bool authmode_detected = false;
    wifi_auth_mode_t authmode = WIFI_AUTH_OPEN;
    esp_netif_ip_info_t acquired_ip = {0};
    uint32_t generation = 0;

    state_lock();
    const bool candidate_ready = s_wifi_candidate.active &&
                                 s_wifi_candidate.attempt_started &&
                                 s_wifi_candidate.commit_pending;
    if (candidate_ready) {
        generation = s_wifi_candidate.generation;
        snprintf(ssid, sizeof(ssid), "%s", s_wifi_candidate.candidate_ssid);
        snprintf(password, sizeof(password), "%s",
                 s_wifi_candidate.candidate_password);
        authmode_detected = s_wifi_candidate.authmode_detected;
        authmode = s_wifi_candidate.authmode;
        acquired_ip = s_wifi_candidate.acquired_ip;
    }
    state_unlock();
    if (!candidate_ready) {
        vTaskDelete(NULL);
        return;
    }

    const esp_err_t save_err = save_credentials(ssid, password,
                                                authmode_detected, authmode);
    secure_clear(password, sizeof(password));
    if (save_err != ESP_OK) {
        memset(ssid, 0, sizeof(ssid));
        rollback_wifi_candidate(save_err, generation);
        vTaskDelete(NULL);
        return;
    }

    bool committed = false;
    bool reconnect = false;
    bool schedule_shutdown = false;
    uint32_t shutdown_generation = 0;
    state_lock();
    /* The NVS write is slow; only confirm this candidate while it is still the
       active generation. A candidate submitted meanwhile owns the state now
       and must not be overwritten (or cleared) by this stale confirmation. */
    if (s_wifi_candidate.active &&
        s_wifi_candidate.generation == generation &&
        s_wifi_candidate.commit_pending) {
        acquired_ip = s_wifi_candidate.acquired_ip;
        memcpy(s_ssid, ssid, sizeof(s_ssid));
        s_authmode = authmode;
        s_authmode_stored = authmode_detected;
        s_connected = s_wifi_candidate.commit_link_connected;
        reconnect = !s_connected;
        s_station_connecting = false;
        s_station_reconnect_paused = false;
        s_station_retry_failures = 0;
        s_last_disconnect_reason = 0;
        s_disconnect_count = 0;
        s_last_connect_error = ESP_OK;
        status_generation_advance_locked();
        schedule_shutdown = s_portal_active &&
                            s_portal_credentials_submitted &&
                            !s_portal_shutdown_scheduled;
        if (schedule_shutdown) {
            s_portal_shutdown_scheduled = true;
            if (++s_portal_shutdown_generation == 0U) {
                ++s_portal_shutdown_generation;
            }
            shutdown_generation = s_portal_shutdown_generation;
        }
        secure_clear(&s_wifi_candidate, sizeof(s_wifi_candidate));
        committed = true;
    }
    state_unlock();

    if (!committed) {
        /* A newer candidate replaced this one during the NVS write; leave its
           state untouched so it can confirm or roll back on its own. */
        memset(ssid, 0, sizeof(ssid));
        vTaskDelete(NULL);
        return;
    }

    if (reconnect) {
        ESP_LOGW(TAG, "Red Wi-Fi confirmada sin enlace; se intentara reconectar");
    } else {
        ESP_LOGI(TAG, "Red Wi-Fi confirmada: %s; IP " IPSTR, ssid,
                 IP2STR(&acquired_ip.ip));
    }
    memset(ssid, 0, sizeof(ssid));
    if (!reconnect) start_time_sync();
    notify_status();
    if (reconnect) schedule_station_retry();
    if (schedule_shutdown &&
        xTaskCreate(portal_shutdown_task, "wifi_portal_stop", 3072,
                    (void *)(uintptr_t)shutdown_generation, 3, NULL) != pdPASS) {
        state_lock();
        /* Only release the flag if no newer shutdown task superseded this one. */
        if (s_portal_shutdown_generation == shutdown_generation) {
            s_portal_shutdown_scheduled = false;
        }
        state_unlock();
        ESP_LOGW(TAG, "No se pudo programar el cierre inactivo del portal");
    }
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        state_lock();
        const bool should_connect = s_ssid[0] != '\0' &&
                                    !s_station_reconnect_paused;
        const bool authmode_stored = s_authmode_stored;
        state_unlock();
        if (should_connect) {
            if (authmode_stored) {
                station_connect_if_idle();
            } else if (xTaskCreate(station_detect_and_connect_task,
                                   "wifi_auth_detect", 4096, NULL, 4,
                                   NULL) != pdPASS) {
                ESP_LOGW(TAG, "No se pudo iniciar la deteccion de seguridad");
                station_connect_if_idle();
            }
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event =
            (const wifi_event_sta_disconnected_t *)event_data;
        char ssid[sizeof(s_ssid)];
        bool candidate_handled = false;
        bool start_candidate = false;
        bool candidate_failed = false;
        bool should_reconnect = false;
        state_lock();
        s_last_disconnect_reason = event->reason;
        if (s_portal_credentials_submitted && s_disconnect_count < UINT8_MAX) {
            s_disconnect_count++;
        }
        s_connected = false;
        status_generation_advance_locked();
        snprintf(ssid, sizeof(ssid), "%s", s_ssid);
        if (s_wifi_candidate.active) {
            candidate_handled = true;
            if (s_wifi_candidate.commit_pending) {
                s_wifi_candidate.commit_link_connected = false;
            }
            if (!s_wifi_candidate.commit_pending) {
                if (s_wifi_candidate.attempt_started) {
                    s_station_connecting = false;
                    s_last_connect_error = ESP_FAIL;
                    candidate_failed = true;
                } else {
                    s_wifi_candidate.link_down = true;
                    s_station_connecting = true;
                    start_candidate = s_wifi_candidate.configuration_ready;
                }
            }
        } else {
            s_station_connecting = false;
            should_reconnect = ssid[0] != '\0' &&
                               !s_station_reconnect_paused;
        }
        state_unlock();
        ESP_LOGW(TAG, "Wi-Fi desconectado de %s; motivo %u", ssid,
                 (unsigned int)event->reason);
        if (candidate_failed) {
            rollback_wifi_candidate(ESP_FAIL, 0);
            return;
        }
        if (candidate_handled && start_candidate) {
            begin_candidate_connection();
            return;
        }
        notify_status();
        if (should_reconnect) {
            schedule_station_retry();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        char ssid[sizeof(s_ssid)];
        bool candidate_handled = false;
        bool start_candidate_commit = false;
        state_lock();
        if (s_wifi_candidate.active) {
            candidate_handled = true;
            if (s_wifi_candidate.attempt_started) {
                s_wifi_candidate.commit_link_connected = true;
                s_wifi_candidate.acquired_ip = event->ip_info;
            }
            if (s_wifi_candidate.attempt_started &&
                !s_wifi_candidate.commit_pending) {
                s_wifi_candidate.commit_pending = true;
                start_candidate_commit = true;
            }
        } else {
            s_connected = true;
            s_station_connecting = false;
            s_station_retry_failures = 0;
            s_last_connect_error = ESP_OK;
            snprintf(ssid, sizeof(ssid), "%s", s_ssid);
            status_generation_advance_locked();
        }
        state_unlock();

        if (candidate_handled) {
            if (start_candidate_commit &&
                xTaskCreate(candidate_commit_task, "wifi_candidate_commit",
                            4096, NULL, 4, NULL) != pdPASS) {
                state_lock();
                s_wifi_candidate.commit_pending = false;
                state_unlock();
                rollback_wifi_candidate(ESP_ERR_NO_MEM, 0);
            } else if (!start_candidate_commit) {
                notify_status();
            }
            return;
        }

        ESP_LOGI(TAG, "Conectado a %s; IP " IPSTR, ssid, IP2STR(&event->ip_info.ip));
        start_time_sync();
        notify_status();
    }
}

static esp_err_t load_credentials(wifi_config_t *config,
                                  bool *needs_snapshot_migration)
{
    if (needs_snapshot_migration != NULL) {
        *needs_snapshot_migration = false;
    }
    memset(&config->sta, 0, sizeof(config->sta));
    memset(s_ssid, 0, sizeof(s_ssid));
    s_authmode = WIFI_AUTH_OPEN;
    s_authmode_stored = false;

    radio_wifi_credentials_load_result_t loaded = {0};
    esp_err_t err = radio_wifi_credentials_store_load(&loaded);
    if (err != ESP_OK) return err;
    if (loaded.legacy_invalid) {
        ESP_LOGW(TAG, "Hay campos Wi-Fi legados invalidos; se conserva NVS");
    }
    if (loaded.has_credentials) {
        if (loaded.credentials.authmode_stored &&
            !authmode_is_usable(
                (wifi_auth_mode_t)loaded.credentials.authmode)) {
            if (loaded.from_snapshot) {
                secure_clear(&loaded, sizeof(loaded));
                return ESP_ERR_INVALID_STATE;
            }
            ESP_LOGW(TAG, "Modo de seguridad Wi-Fi legado invalido; "
                          "se usara el predeterminado");
            loaded.credentials.authmode_stored = false;
            loaded.credentials.authmode = (uint8_t)WIFI_AUTH_OPEN;
        }

        memcpy(s_ssid, loaded.credentials.ssid, sizeof(s_ssid));
        const size_t ssid_length = strnlen(s_ssid, sizeof(s_ssid));
        const size_t password_length =
            strnlen(loaded.credentials.password,
                    sizeof(loaded.credentials.password));
        memcpy(config->sta.ssid, s_ssid, ssid_length);
        memcpy(config->sta.password, loaded.credentials.password,
               password_length);
        if (loaded.credentials.authmode_stored) {
            s_authmode =
                (wifi_auth_mode_t)loaded.credentials.authmode;
            s_authmode_stored = true;
            config->sta.threshold.authmode = s_authmode;
        }
        config->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        config->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        config->sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
        config->sta.pmf_cfg.capable = true;
        config->sta.pmf_cfg.required = false;
        if (needs_snapshot_migration != NULL) {
            *needs_snapshot_migration =
                loaded.needs_snapshot_migration;
        }
    }
    secure_clear(&loaded, sizeof(loaded));
    return ESP_OK;
}

static esp_err_t config_schema_check_compatible(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    uint8_t schema_version = 0;
    err = nvs_get_u8(nvs, WIFI_NVS_SCHEMA_VERSION_KEY, &schema_version);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    return schema_version > WIFI_NVS_SCHEMA_VERSION
               ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
}

typedef struct {
    uint8_t version;
} config_schema_write_t;

static esp_err_t config_schema_write_job(void *context)
{
    const config_schema_write_t *write = context;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    uint8_t schema_version = 0;
    err = nvs_get_u8(nvs, WIFI_NVS_SCHEMA_VERSION_KEY, &schema_version);
    if (err == ESP_ERR_NVS_NOT_FOUND ||
        (err == ESP_OK && schema_version < write->version)) {
        err = nvs_set_u8(nvs, WIFI_NVS_SCHEMA_VERSION_KEY, write->version);
        if (err == ESP_OK) err = RADIO_TEST_NVS_COMMIT(nvs);
    } else if (err == ESP_OK && schema_version > write->version) {
        err = ESP_ERR_NOT_SUPPORTED;
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t config_schema_mark_current(void)
{
    const config_schema_write_t write = {
        .version = WIFI_NVS_SCHEMA_VERSION,
    };
    const esp_err_t err = radio_nvs_worker_run_sync(
        config_schema_write_job, (void *)&write);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Namespace de configuracion NVS en version %u",
                 (unsigned)WIFI_NVS_SCHEMA_VERSION);
    }
    return err;
}

static void prepare_portal_identity(void)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
        memcpy(mac, "ONDA01", sizeof(mac));
    }
    snprintf(s_portal_ssid, sizeof(s_portal_ssid), "ONDA-%02X%02X%02X",
             mac[3], mac[4], mac[5]);
}

static void log_nvs_capacity(void)
{
    nvs_stats_t stats = {0};
    const esp_err_t err = nvs_get_stats(NULL, &stats);
    if (err == ESP_OK) {
        ESP_LOGE(TAG,
                 "NVS capacity: used=%zu available=%zu total=%zu namespaces=%zu",
                 stats.used_entries, stats.available_entries,
                 stats.total_entries, stats.namespace_count);
    } else {
        ESP_LOGE(TAG, "NVS capacity stats unavailable: %s",
                 esp_err_to_name(err));
    }
}

static bool portal_password_is_valid(const char *password)
{
    if (password == NULL || strlen(password) != WIFI_PORTAL_PASSWORD_LENGTH) {
        return false;
    }
    for (size_t i = 0; i < WIFI_PORTAL_PASSWORD_LENGTH; ++i) {
        if (!((password[i] >= '0' && password[i] <= '9') ||
              (password[i] >= 'a' && password[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

typedef struct {
    char password[WIFI_PORTAL_PASSWORD_LENGTH + 1U];
} portal_password_write_t;

static esp_err_t save_portal_password_job(void *context)
{
    const portal_password_write_t *write = context;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_str(nvs, WIFI_PORTAL_PASSWORD_KEY, write->password);
    if (err == ESP_OK) err = RADIO_TEST_NVS_COMMIT(nvs);
    nvs_close(nvs);
    return err;
}

static esp_err_t load_or_create_portal_password(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs);
    bool create_password = false;
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_portal_password[0] = '\0';
        create_password = true;
        err = ESP_OK;
    } else if (err != ESP_OK) {
        return err;
    } else {
        size_t stored_size = sizeof(s_portal_password);
        err = nvs_get_str(nvs, WIFI_PORTAL_PASSWORD_KEY, s_portal_password,
                          &stored_size);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            create_password = true;
            err = ESP_OK;
        } else if (err == ESP_OK &&
                   !portal_password_is_valid(s_portal_password)) {
            /* Fail closed; explicit USB NVS recovery can generate a new secret. */
            err = ESP_ERR_INVALID_STATE;
        }
        nvs_close(nvs);
    }

    if (err == ESP_OK && create_password) {
        static const char hex[] = "0123456789abcdef";
        uint8_t secret[WIFI_PORTAL_PASSWORD_BYTES] = {0};
        esp_fill_random(secret, sizeof(secret));
        for (size_t i = 0; i < sizeof(secret); ++i) {
            s_portal_password[i * 2U] = hex[secret[i] >> 4U];
            s_portal_password[i * 2U + 1U] = hex[secret[i] & 0x0fU];
        }
        s_portal_password[WIFI_PORTAL_PASSWORD_LENGTH] = '\0';
        secure_clear(secret, sizeof(secret));
        portal_password_write_t write = {0};
        memcpy(write.password, s_portal_password, sizeof(write.password));
        err = radio_nvs_worker_run_sync(save_portal_password_job, &write);
        secure_clear(&write, sizeof(write));
    }
    if (err != ESP_OK) {
        memset(s_portal_password, 0, sizeof(s_portal_password));
    }
    return err;
}

esp_err_t radio_wifi_init(void)
{
    if (s_state_lock == NULL) {
        s_state_lock = xSemaphoreCreateMutex();
        if (s_state_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_portal_lifecycle_lock == NULL) {
        s_portal_lifecycle_lock = xSemaphoreCreateMutex();
        if (s_portal_lifecycle_lock == NULL) return ESP_ERR_NO_MEM;
    }
    state_lock();
    const bool already_initialized = s_initialized;
    state_unlock();
    if (already_initialized) {
        return ESP_OK;
    }
    if (s_ap_netif != NULL) {
        ESP_LOGE(TAG, "No se puede reintentar Wi-Fi tras una limpieza incompleta");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(config_schema_check_compatible(), TAG,
                        "Version NVS de Wi-Fi no compatible");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "No se pudo inicializar TCP/IP");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == NULL) {
        return ESP_FAIL;
    }
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) {
        esp_netif_destroy_default_wifi(sta_netif);
        return ESP_FAIL;
    }

    bool wifi_initialized = false;
    bool wifi_handler_registered = false;
    bool ip_handler_registered = false;
    bool credentials_snapshot_migration = false;
    esp_err_t ret = ESP_OK;
    wifi_config_t sta_config = {0};
    const wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_GOTO_ON_ERROR(esp_wifi_init(&init_config), fail, TAG, "No se pudo iniciar Wi-Fi");
    wifi_initialized = true;
    ESP_GOTO_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                  wifi_event_handler, NULL),
                      fail, TAG, "No se pudieron registrar eventos Wi-Fi");
    wifi_handler_registered = true;
    ESP_GOTO_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                  wifi_event_handler, NULL),
                      fail, TAG, "No se pudieron registrar eventos IP");
    ip_handler_registered = true;
    ESP_GOTO_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), fail, TAG,
                      "No se pudo configurar almacenamiento Wi-Fi");

    ESP_GOTO_ON_ERROR(load_credentials(&sta_config,
                                       &credentials_snapshot_migration),
                      fail, TAG,
                      "No se pudieron leer las credenciales guardadas");
    if (credentials_snapshot_migration) {
        ESP_GOTO_ON_ERROR(migrate_wifi_snapshot(&sta_config), fail, TAG,
                          "No se pudieron migrar las credenciales Wi-Fi");
    }
    prepare_portal_identity();
    const esp_err_t portal_password_err = load_or_create_portal_password();
    if (portal_password_err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        log_nvs_capacity();
    }
    ESP_GOTO_ON_ERROR(portal_password_err, fail, TAG,
                      "No se pudo cargar o generar la credencial del portal");
    ESP_GOTO_ON_ERROR(config_schema_mark_current(), fail, TAG,
                      "No se pudo confirmar la migracion de configuracion NVS");
    ESP_GOTO_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), fail, TAG,
                      "No se pudo configurar modo Wi-Fi");
    ESP_GOTO_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_config), fail, TAG,
                      "No se pudo configurar la red Wi-Fi");
    secure_clear(&sta_config, sizeof(sta_config));
    ESP_GOTO_ON_ERROR(esp_wifi_start(), fail, TAG, "No se pudo iniciar la radio Wi-Fi");

    state_lock();
    s_initialized = true;
    status_generation_advance_locked();
    const bool needs_provisioning = s_ssid[0] == '\0';
    state_unlock();
    if (needs_provisioning) {
        ESP_RETURN_ON_ERROR(radio_wifi_start_provisioning(), TAG,
                            "No se pudo iniciar configuracion por celular");
    }
    notify_status();
    return ESP_OK;

fail:
    secure_clear(&sta_config, sizeof(sta_config));
    bool cleanup_safe = true;
    if (ip_handler_registered) {
        const esp_err_t cleanup_err = esp_event_handler_unregister(
            IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler);
        if (cleanup_err != ESP_OK) {
            cleanup_safe = false;
            ESP_LOGW(TAG, "No se pudo liberar el manejador IP: %s",
                     esp_err_to_name(cleanup_err));
        }
    }
    if (wifi_handler_registered) {
        const esp_err_t cleanup_err = esp_event_handler_unregister(
            WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler);
        if (cleanup_err != ESP_OK) {
            cleanup_safe = false;
            ESP_LOGW(TAG, "No se pudo liberar el manejador Wi-Fi: %s",
                     esp_err_to_name(cleanup_err));
        }
    }
    if (wifi_initialized) {
        const esp_err_t cleanup_err = esp_wifi_deinit();
        if (cleanup_err != ESP_OK) {
            cleanup_safe = false;
            ESP_LOGW(TAG, "No se pudo liberar el driver Wi-Fi: %s",
                     esp_err_to_name(cleanup_err));
        }
    }
    if (cleanup_safe) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
        esp_netif_destroy_default_wifi(sta_netif);
    } else {
        ESP_LOGE(TAG, "Se conservan interfaces Wi-Fi tras limpieza incompleta");
    }
    return ret;
}

static esp_err_t start_provisioning_locked(void)
{
    state_lock();
    const bool initialized = s_initialized;
    const bool active = s_portal_active;
    const bool station_connected = s_connected;
    state_unlock();
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (active) {
        notify_status();
        return ESP_OK;
    }

    wifi_config_t ap_config = {0};
    memcpy(ap_config.ap.ssid, s_portal_ssid, strlen(s_portal_ssid));
    ap_config.ap.ssid_len = strlen(s_portal_ssid);
    memcpy(ap_config.ap.password, s_portal_password, strlen(s_portal_password));
    ap_config.ap.channel = 1;
    if (station_connected) {
        wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
        uint8_t station_channel = 0;
        if (esp_wifi_get_channel(&station_channel, &secondary) == ESP_OK &&
            station_channel >= 1U && station_channel <= 14U) {
            ap_config.ap.channel = station_channel;
        }
    }
    ap_config.ap.max_connection = 2;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_config.ap.pmf_cfg.required = false;

    state_lock();
    s_station_reconnect_paused = false;
    state_unlock();
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        return recover_station_after_portal_error(err);
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK) {
        return recover_station_after_portal_error(err);
    }
    err = start_http_server();
    if (err != ESP_OK) {
        return recover_station_after_portal_error(err);
    }

    state_lock();
    s_portal_active = true;
    status_generation_advance_locked();
    s_portal_credentials_submitted = false;
    s_portal_shutdown_scheduled = false;
    state_unlock();
    ESP_LOGI(TAG, "Portal listo: red %s; abrir http://192.168.4.1", s_portal_ssid);
    notify_status();
    return ESP_OK;
}

esp_err_t radio_wifi_start_provisioning(void)
{
    if (s_portal_lifecycle_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_portal_lifecycle_lock, portMAX_DELAY);
    const esp_err_t err = start_provisioning_locked();
    xSemaphoreGive(s_portal_lifecycle_lock);
    return err;
}

static esp_err_t save_and_connect(const char *ssid, const char *password,
                                  int authmode, bool from_portal)
{
    state_lock();
    const bool initialized = s_initialized;
    state_unlock();
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ssid == NULL || password == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t ssid_len = strnlen(ssid, sizeof(s_ssid));
    const size_t password_len = strnlen(password, sizeof(((wifi_config_t *)0)->sta.password));
    if (ssid_len == 0 || ssid_len > 32 || password_len > 64) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_auth_mode_t selected_authmode = WIFI_AUTH_OPEN;
    bool authmode_detected = false;
    if (authmode >= 0 && authmode < WIFI_AUTH_MAX &&
        authmode_is_usable((wifi_auth_mode_t)authmode)) {
        selected_authmode = (wifi_auth_mode_t)authmode;
        authmode_detected = true;
    } else if (detect_authmode(ssid, &selected_authmode) == ESP_OK) {
        authmode_detected = true;
        ESP_LOGI(TAG, "Seguridad detectada para %s: %s", ssid,
                 wifi_auth_label(selected_authmode));
    } else {
        selected_authmode = password_len == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
        ESP_LOGW(TAG, "No se detecto la red %s durante el escaneo; se probara %s",
                 ssid, wifi_auth_label(selected_authmode));
    }

    wifi_config_t config = {0};
    memcpy(config.sta.ssid, ssid, ssid_len);
    memcpy(config.sta.password, password, password_len);
    config.sta.threshold.authmode = selected_authmode;
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    wifi_config_t previous_config = {0};
    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &previous_config);
    if (err != ESP_OK) {
        secure_clear(&previous_config, sizeof(previous_config));
        secure_clear(&config, sizeof(config));
        return err;
    }

    state_lock();
    if (s_wifi_candidate.active) {
        state_unlock();
        secure_clear(&previous_config, sizeof(previous_config));
        secure_clear(&config, sizeof(config));
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_wifi_candidate, 0, sizeof(s_wifi_candidate));
    s_wifi_candidate.active = true;
    if (from_portal) s_portal_credentials_submitted = true;
    if (++s_wifi_candidate_generation == 0U) ++s_wifi_candidate_generation;
    s_wifi_candidate.generation = s_wifi_candidate_generation;
    s_wifi_candidate.link_down = !s_connected && !s_station_connecting;
    s_wifi_candidate.authmode_detected = authmode_detected;
    s_wifi_candidate.authmode = selected_authmode;
    s_wifi_candidate.previous_config = previous_config;
    memcpy(s_wifi_candidate.candidate_ssid, ssid, ssid_len);
    memcpy(s_wifi_candidate.candidate_password, password, password_len);
    s_wifi_candidate.candidate_ssid[ssid_len] = '\0';
    s_wifi_candidate.candidate_password[password_len] = '\0';
    s_station_reconnect_paused = true;
    s_connected = false;
    status_generation_advance_locked();
    s_station_connecting = true;
    s_station_retry_failures = 0;
    s_last_disconnect_reason = 0;
    s_disconnect_count = 0;
    s_last_connect_error = ESP_OK;
    state_unlock();

    err = esp_wifi_disconnect();
    if (err == ESP_ERR_WIFI_NOT_CONNECT || err == ESP_ERR_WIFI_STATE) {
        state_lock();
        if (s_wifi_candidate.active) s_wifi_candidate.link_down = true;
        state_unlock();
    } else if (err != ESP_OK) {
        rollback_wifi_candidate(err, 0);
        secure_clear(&previous_config, sizeof(previous_config));
        secure_clear(&config, sizeof(config));
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err != ESP_OK) {
        rollback_wifi_candidate(err, 0);
        secure_clear(&previous_config, sizeof(previous_config));
        secure_clear(&config, sizeof(config));
        return err;
    }

    bool start_candidate = false;
    state_lock();
    if (s_wifi_candidate.active) {
        s_wifi_candidate.configuration_ready = true;
        start_candidate = s_wifi_candidate.link_down;
    }
    state_unlock();
    secure_clear(&previous_config, sizeof(previous_config));
    secure_clear(&config, sizeof(config));
    if (start_candidate) {
        begin_candidate_connection();
    } else {
        notify_status();
    }
    return ESP_OK;
}

esp_err_t radio_wifi_save_and_connect(const char *ssid, const char *password,
                                      int authmode)
{
    return save_and_connect(ssid, password, authmode, false);
}

bool radio_wifi_is_connected(void)
{
    state_lock();
    const bool connected = s_connected;
    state_unlock();
    return connected;
}

esp_err_t radio_wifi_get_info(radio_wifi_info_t *info)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    state_lock();
    get_info_locked(info);
    state_unlock();
    return ESP_OK;
}

esp_err_t radio_wifi_get_portal_credentials(char *ssid, size_t ssid_capacity,
                                            char *password,
                                            size_t password_capacity)
{
    if (ssid == NULL || password == NULL || ssid_capacity == 0U ||
        password_capacity == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    ssid[0] = '\0';
    password[0] = '\0';

    state_lock();
    if (!s_portal_active) {
        state_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    const size_t ssid_length = strnlen(s_portal_ssid, sizeof(s_portal_ssid));
    const size_t password_length =
        strnlen(s_portal_password, sizeof(s_portal_password));
    if (ssid_length == 0U || password_length == 0U ||
        ssid_length >= ssid_capacity || password_length >= password_capacity) {
        state_unlock();
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(ssid, s_portal_ssid, ssid_length + 1U);
    memcpy(password, s_portal_password, password_length + 1U);
    state_unlock();
    return ESP_OK;
}

void radio_wifi_set_status_callback(radio_wifi_status_cb_t callback)
{
    state_lock();
    s_status_callback = callback;
    state_unlock();
    notify_status();
}
