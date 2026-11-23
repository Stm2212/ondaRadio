#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_aac_dec.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "radio_board.h"
#include "radio_player.h"
#include "radio_lock_diagnostics.h"
#include "radio_stream_policy.h"
#include "radio_stream_watchdog.h"
#include "radio_test_adapters.h"
#include "radio_task_supervisor.h"
#include "radio_station_store.h"
#include "radio_wifi.h"

/* The policy classifier uses numeric copies of these codes to stay free of the
   codec dependency; fail the build if the component ever renumbers them. */
_Static_assert(ESP_AUDIO_ERR_MEM_LACK == RADIO_STREAM_DECODER_ERR_MEM_LACK,
               "esp_audio MEM_LACK code changed");
_Static_assert(ESP_AUDIO_ERR_BUFF_NOT_ENOUGH ==
                   RADIO_STREAM_DECODER_ERR_BUFF_NOT_ENOUGH,
               "esp_audio BUFF_NOT_ENOUGH code changed");

#define PLAYER_INPUT_SIZE 2048
#define PLAYER_OUTPUT_SIZE 16384
#define PLAYER_PENDING_MAX 8192
#define PLAYER_PCM_BUFFER_SIZE (192U * 1024U)
#define PLAYER_PCM_MIN_BUFFER_SIZE (64U * 1024U)
#define PLAYER_PCM_SINK_SIZE 4096U
#define PLAYER_AUDIO_TASK_STACK_SIZE 6144U
#define PLAYER_PCM_PREBUFFER_MIN (16U * 1024U)
#define PLAYER_PCM_PREBUFFER_MAX (48U * 1024U)
#define PLAYER_PCM_REBUFFER_DIVISOR 2U
#define PLAYER_PCM_LOG_INTERVAL_US (15LL * 1000LL * 1000LL)
#define PLAYER_STATUS_MESSAGE_MAX 72
#define PLAYER_HTTP_CONNECT_TIMEOUT_MS 8000
#define PLAYER_HTTP_READ_TIMEOUT_MS 800

typedef struct {
    esp_audio_simple_dec_handle_t decoder;
    StreamBufferHandle_t pcm_stream;
    uint8_t *output_buffer;
    size_t output_capacity;
    uint8_t *input_buffer;
    size_t input_capacity;
    size_t input_length;
    uint32_t generation;
    bool audio_open;
    bool saw_audio;
    bool stop_requested;
    bool decode_error;
    bool audio_sink_started;
    bool producer_finished;
    bool sink_abort_requested;
    esp_err_t sink_error;
    radio_stream_watchdog_t *watchdog;
    uint8_t frame_bytes;
    uint64_t pcm_produced_bytes;
    uint64_t pcm_consumed_bytes;
    size_t pcm_peak_bytes;
    size_t pcm_min_bytes;
    uint32_t pcm_underruns;
    uint32_t pcm_rebuffer_events;
    uint32_t sink_stack_high_water;
    uint32_t pcm_bytes_per_second;
    const char *format_name;
    uint32_t http_read_max_us;
    uint32_t decode_max_us;
    uint32_t i2s_write_max_us;
    uint32_t http_read_eagain;
    int64_t metrics_last_log_us;
} stream_context_t;

static const char *TAG = "radio_player";
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_volume_queue;
static QueueHandle_t s_status_queue;
static SemaphoreHandle_t s_audio_sink_done;
static TaskHandle_t s_worker;
static TaskHandle_t s_volume_worker;
static StreamBufferHandle_t s_pcm_stream;
static StaticStreamBuffer_t s_pcm_stream_state;
static uint8_t *s_pcm_storage;
static size_t s_pcm_capacity;
static uint8_t s_pcm_sink_buffer[PLAYER_PCM_SINK_SIZE];
static radio_station_t s_pending_station;
static radio_station_t s_current_station;
static bool s_has_pending_station;
static bool s_stop_requested;
static bool s_pause_requested;
static bool s_decoder_initialized;
static bool s_initialized;
static int s_volume = 55;
static bool s_volume_persist_pending;
static radio_player_state_t s_state = RADIO_PLAYER_STATE_IDLE;
static char s_status_message[PLAYER_STATUS_MESSAGE_MAX] = "En espera";
static uint32_t s_status_sequence;
static uint32_t s_play_generation;
static radio_player_status_cb_t s_status_callback;

static int64_t stream_monotonic_clock(void *context)
{
    (void)context;
    return RADIO_TEST_CLOCK_US(esp_timer_get_time());
}

typedef struct {
    radio_player_status_cb_t callback;
    radio_player_state_t state;
    uint32_t sequence;
    char station[RADIO_STATION_NAME_MAX];
    char message[PLAYER_STATUS_MESSAGE_MAX];
} player_status_event_t;

static void status_event_update_locked(player_status_event_t *event,
                                       radio_player_state_t state,
                                       const char *message)
{
    s_state = state;
    snprintf(s_status_message, sizeof(s_status_message), "%s",
             message != NULL ? message : "");
    ++s_status_sequence;
    if (s_status_sequence == 0) {
        ++s_status_sequence;
    }
    event->callback = s_status_callback;
    event->state = state;
    event->sequence = s_status_sequence;
    snprintf(event->station, sizeof(event->station), "%s", s_current_station.name);
    snprintf(event->message, sizeof(event->message), "%s", s_status_message);
}

static void status_event_deliver(const player_status_event_t *event)
{
    if (event->callback == NULL || s_lock == NULL) {
        return;
    }
    (void)radio_mutex_take_audited(s_lock, portMAX_DELAY, TAG,
                                   "player", __func__);
    const bool current = event->sequence == s_status_sequence &&
                         event->callback == s_status_callback;
    xSemaphoreGive(s_lock);
    if (current) {
        event->callback(event->state, event->sequence, event->station,
                        event->message);
    }
}

static void status_event_dispatch(const player_status_event_t *event)
{
    if (event == NULL || event->callback == NULL || s_status_queue == NULL ||
        s_lock == NULL) {
        return;
    }

    /* A delayed publisher must not replace a newer event in the mailbox.
       Overwrite never waits; delivery and callbacks still happen outside the lock. */
    (void)radio_mutex_take_audited(s_lock, portMAX_DELAY, TAG,
                                   "player", __func__);
    const bool current = event->sequence == s_status_sequence &&
                         event->callback == s_status_callback;
    if (current) (void)RADIO_TEST_QUEUE_OVERWRITE(s_status_queue, event);
    xSemaphoreGive(s_lock);
}

static void volume_worker(void *arg)
{
    (void)arg;
    uint8_t request;
    for (;;) {
        player_status_event_t status_event;
        if (s_status_queue != NULL &&
            xQueueReceive(s_status_queue, &status_event, 0) == pdTRUE) {
            status_event_deliver(&status_event);
            continue;
        }
        if (xQueueReceive(s_volume_queue, &request, pdMS_TO_TICKS(10)) != pdTRUE) {
            continue;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        const int volume = s_volume;
        const bool persist = s_volume_persist_pending;
        s_volume_persist_pending = false;
        xSemaphoreGive(s_lock);

        const esp_err_t volume_err = radio_board_audio_set_volume(volume);
        if (volume_err != ESP_OK) {
            ESP_LOGW(TAG, "No se pudo aplicar el volumen: %s",
                     esp_err_to_name(volume_err));
        }
        if (persist) {
            const esp_err_t err = radio_station_store_set_volume(volume);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "No se pudo programar el guardado del volumen: %s",
                         esp_err_to_name(err));
            }
        }
    }
}

static void volume_schedule_apply(bool persist)
{
    if (s_lock == NULL || s_volume_queue == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_volume_persist_pending = s_volume_persist_pending || persist;
    xSemaphoreGive(s_lock);

    /* A one-slot queue coalesces rapid slider and button changes. */
    const uint8_t request = 1;
    (void)RADIO_TEST_QUEUE_OVERWRITE(s_volume_queue, &request);
}

static uint32_t next_generation(uint32_t generation)
{
    ++generation;
    return generation != 0 ? generation : 1;
}

static void notify_status_for_generation(uint32_t generation,
                                         radio_player_state_t state,
                                         const char *message)
{
    player_status_event_t event = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool current = generation == s_play_generation && !s_stop_requested;
    if (current) {
        status_event_update_locked(&event, state, message);
    }
    xSemaphoreGive(s_lock);
    if (current) {
        status_event_dispatch(&event);
    }
}

static bool stop_requested(void)
{
    if (s_lock == NULL) {
        return true;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool requested = s_stop_requested;
    xSemaphoreGive(s_lock);
    return requested;
}

static bool stream_is_finished(stream_context_t *stream)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool finished = stream->producer_finished;
    xSemaphoreGive(s_lock);
    return finished;
}

static bool stream_sink_is_aborted(stream_context_t *stream)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool aborted = stream->sink_abort_requested;
    xSemaphoreGive(s_lock);
    return aborted;
}

static void stream_abort_sink(stream_context_t *stream)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stream->sink_abort_requested = true;
    stream->producer_finished = true;
    xSemaphoreGive(s_lock);
    radio_board_audio_mute();
}

static esp_err_t stream_sink_error(stream_context_t *stream)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const esp_err_t err = stream->sink_error;
    xSemaphoreGive(s_lock);
    return err;
}

static void stream_finish_producer(stream_context_t *stream)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stream->producer_finished = true;
    xSemaphoreGive(s_lock);
}

static void increment_u32_saturating(uint32_t *value)
{
    if (*value < UINT32_MAX) ++*value;
}

static void add_u64_saturating(uint64_t *value, size_t amount)
{
    const uint64_t increment = (uint64_t)amount;
    *value = UINT64_MAX - *value < increment ? UINT64_MAX
                                              : *value + increment;
}

static void stream_set_sink_error(stream_context_t *stream, esp_err_t err)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (stream->sink_error == ESP_OK) stream->sink_error = err;
    xSemaphoreGive(s_lock);
}

static void stream_note_produced(stream_context_t *stream, size_t bytes)
{
    const size_t fill = xStreamBufferBytesAvailable(stream->pcm_stream);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    add_u64_saturating(&stream->pcm_produced_bytes, bytes);
    if (fill > stream->pcm_peak_bytes) stream->pcm_peak_bytes = fill;
    if (fill < stream->pcm_min_bytes) stream->pcm_min_bytes = fill;
    xSemaphoreGive(s_lock);
}

static void stream_note_consumed(stream_context_t *stream, size_t bytes,
                                 size_t fill_after)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    add_u64_saturating(&stream->pcm_consumed_bytes, bytes);
    if (fill_after < stream->pcm_min_bytes) stream->pcm_min_bytes = fill_after;
    xSemaphoreGive(s_lock);
}

static void stream_note_underrun(stream_context_t *stream)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    increment_u32_saturating(&stream->pcm_underruns);
    increment_u32_saturating(&stream->pcm_rebuffer_events);
    stream->pcm_min_bytes = 0;
    xSemaphoreGive(s_lock);
}

static void stream_note_preventive_rebuffer(stream_context_t *stream,
                                            size_t remaining_bytes)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    increment_u32_saturating(&stream->pcm_rebuffer_events);
    if (remaining_bytes < stream->pcm_min_bytes) {
        stream->pcm_min_bytes = remaining_bytes;
    }
    xSemaphoreGive(s_lock);
}

static void stream_note_max_duration(stream_context_t *stream, uint32_t *maximum,
                                     int64_t duration_us)
{
    const uint32_t duration = duration_us <= 0 ? 0U :
                              duration_us > UINT32_MAX ? UINT32_MAX :
                              (uint32_t)duration_us;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (duration > *maximum) *maximum = duration;
    xSemaphoreGive(s_lock);
}

static void audio_metrics_log(stream_context_t *stream, bool force)
{
    const int64_t now = esp_timer_get_time();
    if (!force && now - stream->metrics_last_log_us < PLAYER_PCM_LOG_INTERVAL_US) {
        return;
    }
    stream->metrics_last_log_us = now;

    uint64_t produced;
    uint64_t consumed;
    size_t peak;
    size_t minimum;
    uint32_t underruns;
    uint32_t rebuffer_events;
    uint32_t sink_stack;
    uint32_t read_max_us;
    uint32_t decode_max_us;
    uint32_t write_max_us;
    uint32_t read_eagain;
    uint32_t bytes_per_second;
    const uint64_t pcm_progress_gap_max_us =
        radio_stream_watchdog_max_progress_gap_us(stream->watchdog);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    produced = stream->pcm_produced_bytes;
    consumed = stream->pcm_consumed_bytes;
    peak = stream->pcm_peak_bytes;
    minimum = stream->pcm_min_bytes;
    underruns = stream->pcm_underruns;
    rebuffer_events = stream->pcm_rebuffer_events;
    sink_stack = stream->sink_stack_high_water;
    read_max_us = stream->http_read_max_us;
    decode_max_us = stream->decode_max_us;
    write_max_us = stream->i2s_write_max_us;
    read_eagain = stream->http_read_eagain;
    bytes_per_second = stream->pcm_bytes_per_second;
    xSemaphoreGive(s_lock);

    const size_t spiram_free = heap_caps_get_free_size(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t spiram_largest = heap_caps_get_largest_free_block(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t internal_free = heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_largest = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    ESP_LOGI(TAG,
             "PCM %llu/%llu KiB; cola pico/min %u/%u KiB (%u/%u ms); rebuffer %u (vacios %u); max gap PCM/read/decode+cola/I2S %u/%u/%u/%u ms; EAGAIN %u; pila sink libre %u B; PSRAM libre/mayor %u/%u KiB; SRAM libre/mayor %u/%u KiB",
             (unsigned long long)(produced / 1024U),
             (unsigned long long)(consumed / 1024U),
             (unsigned)(peak / 1024U), (unsigned)(minimum / 1024U),
             bytes_per_second != 0 ? (unsigned)((uint64_t)peak * 1000U / bytes_per_second) : 0U,
              bytes_per_second != 0 ? (unsigned)((uint64_t)minimum * 1000U / bytes_per_second) : 0U,
              (unsigned)rebuffer_events, (unsigned)underruns,
              (unsigned)(pcm_progress_gap_max_us / 1000U),
              (unsigned)(read_max_us / 1000U), (unsigned)(decode_max_us / 1000U),
             (unsigned)(write_max_us / 1000U), (unsigned)read_eagain,
             (unsigned)sink_stack,
             (unsigned)(spiram_free / 1024U),
             (unsigned)(spiram_largest / 1024U),
             (unsigned)(internal_free / 1024U),
             (unsigned)(internal_largest / 1024U));
}

static esp_err_t pcm_enqueue(stream_context_t *stream, const uint8_t *data,
                             size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        if (stop_requested()) return ESP_ERR_INVALID_STATE;
        const esp_err_t sink_err = stream_sink_error(stream);
        if (sink_err != ESP_OK) return sink_err;

        const size_t sent = xStreamBufferSend(stream->pcm_stream, data + offset,
                                              length - offset,
                                              pdMS_TO_TICKS(30));
        if (sent == 0) continue;
        offset += sent;
        stream_note_produced(stream, sent);
    }
    return ESP_OK;
}

static bool audio_output_start(stream_context_t *stream)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool current = !s_stop_requested &&
                         !stream->sink_abort_requested &&
                         stream->generation == s_play_generation;
    if (current) radio_board_audio_unmute();
    xSemaphoreGive(s_lock);
    return current;
}

static void audio_sink_task(void *arg)
{
    stream_context_t *stream = arg;
    const uint32_t supervisor_registration =
        radio_task_supervisor_register(RADIO_SUPERVISED_TASK_AUDIO_SINK);
    const size_t prebuffer = s_pcm_capacity / 4U < PLAYER_PCM_PREBUFFER_MIN
                                 ? PLAYER_PCM_PREBUFFER_MIN
                                 : (s_pcm_capacity / 4U > PLAYER_PCM_PREBUFFER_MAX
                                        ? PLAYER_PCM_PREBUFFER_MAX
                                        : s_pcm_capacity / 4U);
    const size_t rebuffer = prebuffer / PLAYER_PCM_REBUFFER_DIVISOR;
    const size_t low_watermark = rebuffer / 2U;
    bool output_started = false;
    bool amplifier_enabled = false;
    bool has_rebuffered = false;
    bool status_reported = false;
    memset(s_pcm_sink_buffer, 0, sizeof(s_pcm_sink_buffer));

    for (;;) {
        radio_task_supervisor_progress(RADIO_SUPERVISED_TASK_AUDIO_SINK);
        if (stop_requested() || stream_sink_is_aborted(stream)) break;
        const esp_err_t sink_err = stream_sink_error(stream);
        if (sink_err != ESP_OK) break;

        const size_t available = xStreamBufferBytesAvailable(stream->pcm_stream);
        const bool finished = stream_is_finished(stream);
        if (output_started && amplifier_enabled && !finished && available > 0 &&
            available < low_watermark) {
            /* Preserve the remaining PCM, mute early, and refill with hysteresis. */
            radio_board_audio_mute();
            amplifier_enabled = false;
            output_started = false;
            has_rebuffered = true;
            stream_note_preventive_rebuffer(stream, available);
        }
        if (!output_started) {
            const size_t start_level = has_rebuffered ? rebuffer : prebuffer;
            if (available < start_level && !finished) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (available == 0 && finished) break;
            output_started = true;
        }

        if (available == 0) {
            if (finished) break;
            /* Mute first, then replace any stale DMA payload with silence. */
            radio_board_audio_mute();
            amplifier_enabled = false;
            output_started = false;
            has_rebuffered = true;
            stream_note_underrun(stream);
            memset(s_pcm_sink_buffer, 0, sizeof(s_pcm_sink_buffer));
            const esp_err_t err = radio_board_audio_write(s_pcm_sink_buffer,
                                                          sizeof(s_pcm_sink_buffer));
            if (err != ESP_OK) {
                stream_set_sink_error(stream, err);
                break;
            }
            continue;
        }

        size_t request = available < sizeof(s_pcm_sink_buffer)
                             ? available : sizeof(s_pcm_sink_buffer);
        if (stream->frame_bytes > 1U) request -= request % stream->frame_bytes;
        if (request == 0) {
            if (finished) break;
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        const size_t received = xStreamBufferReceive(stream->pcm_stream,
                                                     s_pcm_sink_buffer,
                                                     request, 0);
        if (received == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        const int64_t write_started_us = esp_timer_get_time();
        const esp_err_t write_err = radio_board_audio_write(s_pcm_sink_buffer,
                                                            received);
        if (write_err != ESP_OK) {
            stream_set_sink_error(stream, write_err);
            break;
        }
        stream_note_max_duration(stream, &stream->i2s_write_max_us,
                                 esp_timer_get_time() - write_started_us);
        stream_note_consumed(stream, received,
                             xStreamBufferBytesAvailable(stream->pcm_stream));
        if (!amplifier_enabled) {
            /* Prime I2S with fresh PCM before making the speaker audible. */
            if (!audio_output_start(stream)) break;
            amplifier_enabled = true;
            if (!status_reported) {
                notify_status_for_generation(stream->generation,
                                             RADIO_PLAYER_STATE_PLAYING,
                                             "Reproduciendo");
                status_reported = true;
            }
        }
    }

    radio_board_audio_mute();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stream->sink_stack_high_water = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_audio_sink_done);
    radio_task_supervisor_unregister(RADIO_SUPERVISED_TASK_AUDIO_SINK,
                                     supervisor_registration);
    vTaskDelete(NULL);
}

static esp_err_t audio_sink_start(stream_context_t *stream)
{
    if (s_pcm_stream == NULL || s_audio_sink_done == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xStreamBufferReset(s_pcm_stream) != pdPASS) {
        return ESP_ERR_INVALID_STATE;
    }
    (void)xSemaphoreTake(s_audio_sink_done, 0);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stream->producer_finished = false;
    stream->sink_abort_requested = false;
    stream->sink_error = ESP_OK;
    stream->pcm_produced_bytes = 0;
    stream->pcm_consumed_bytes = 0;
    stream->pcm_peak_bytes = 0;
    stream->pcm_min_bytes = s_pcm_capacity;
    stream->pcm_underruns = 0;
    stream->pcm_rebuffer_events = 0;
    stream->sink_stack_high_water = 0;
    xSemaphoreGive(s_lock);

    const size_t internal_free =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "Reservando salida I2S: stack %u B, interna libre %u B, bloque mayor %u B",
             (unsigned)PLAYER_AUDIO_TASK_STACK_SIZE, (unsigned)internal_free,
             (unsigned)internal_largest);
    if (xTaskCreate(audio_sink_task, "radio_audio",
                    PLAYER_AUDIO_TASK_STACK_SIZE, stream, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Sin RAM interna contigua para la tarea de salida I2S");
        return ESP_ERR_NO_MEM;
    }
    stream->audio_sink_started = true;
    return ESP_OK;
}

static esp_err_t decode_chunk(stream_context_t *stream, const uint8_t *data,
                              size_t length)
{
    if (stream->input_length + length > PLAYER_PENDING_MAX) {
        stream->decode_error = true;
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t required = stream->input_length + length;
    if (required > stream->input_capacity) return ESP_ERR_NO_MEM;
    memcpy(stream->input_buffer + stream->input_length, data, length);
    stream->input_length += length;

    esp_audio_simple_dec_raw_t raw = {
        .buffer = stream->input_buffer,
        .len = stream->input_length,
        .eos = false,
    };
    while (raw.len > 0) {
        esp_audio_simple_dec_out_t frame = {
            .buffer = stream->output_buffer,
            .len = stream->output_capacity,
        };
        raw.consumed = 0;
        const esp_audio_err_t decode_result =
            esp_audio_simple_dec_process(stream->decoder, &raw, &frame);
        if (radio_stream_decoder_error_is_resource((int)decode_result)) {
            /* A buffer or allocation shortage is a resource failure, not a
               format error. Leave decode_error unset so the caller classifies
               it as RADIO_STREAM_FAILURE_RESOURCE and reports a memory problem
               instead of an incompatible format. */
            return ESP_ERR_NO_MEM;
        }
        if (decode_result != ESP_AUDIO_ERR_OK) {
            stream->decode_error = true;
            return ESP_ERR_NOT_SUPPORTED;
        }

        if (frame.decoded_size > 0) {
            if (!stream->audio_open) {
                esp_audio_simple_dec_info_t info = {0};
                if (esp_audio_simple_dec_get_info(stream->decoder, &info) != ESP_AUDIO_ERR_OK) {
                    return ESP_ERR_INVALID_STATE;
                }
                if (info.bits_per_sample != 16 || info.channel == 0 ||
                    info.channel > 2) {
                    stream->decode_error = true;
                    return ESP_ERR_NOT_SUPPORTED;
                }
                esp_err_t err = radio_board_audio_open(info.sample_rate, info.channel,
                                                       info.bits_per_sample);
                if (err != ESP_OK) {
                    return err;
                }
                stream->audio_open = true;
                stream->frame_bytes = (uint8_t)(info.channel *
                                                (info.bits_per_sample / 8U));
                stream->pcm_bytes_per_second = info.sample_rate * stream->frame_bytes;
                radio_board_audio_mute();
                err = audio_sink_start(stream);
                if (err != ESP_OK) return err;
                ESP_LOGI(TAG, "%s: %lu Hz, %u canales, %u bits",
                         stream->format_name != NULL ? stream->format_name : "Audio",
                         (unsigned long)info.sample_rate, info.channel,
                         info.bits_per_sample);
            }
            if (stream->frame_bytes == 0 ||
                frame.decoded_size % stream->frame_bytes != 0) {
                stream->decode_error = true;
                return ESP_ERR_INVALID_SIZE;
            }
            if (radio_stream_watchdog_check(stream->watchdog) !=
                RADIO_STREAM_WATCHDOG_OK) {
                return ESP_ERR_TIMEOUT;
            }
            radio_stream_watchdog_note_progress(stream->watchdog);
            radio_task_supervisor_progress(
                RADIO_SUPERVISED_TASK_AUDIO_PLAYER);
            esp_err_t err = pcm_enqueue(stream, frame.buffer, frame.decoded_size);
            if (err != ESP_OK) {
                return err;
            }
            stream->saw_audio = true;
        }

        if (raw.consumed == 0) {
            break;
        }
        if (raw.consumed > stream->input_length) {
            stream->decode_error = true;
            return ESP_ERR_INVALID_RESPONSE;
        }
        stream->input_length -= raw.consumed;
        memmove(stream->input_buffer, stream->input_buffer + raw.consumed,
                stream->input_length);
        raw.buffer = stream->input_buffer;
        raw.len = stream->input_length;
    }
    return ESP_OK;
}

static bool is_redirect_status(int status_code)
{
    return status_code == 301 || status_code == 302 || status_code == 303 ||
           status_code == 307 || status_code == 308;
}

static void stream_failure_set(char *message, size_t capacity,
                               const char *description)
{
    if (message == NULL || capacity == 0 || description == NULL) return;
    snprintf(message, capacity, "%s", description);
}

static esp_err_t stream_watchdog_check(
    const radio_stream_watchdog_t *watchdog,
    char *failure_message, size_t failure_capacity)
{
    switch (radio_stream_watchdog_check(watchdog)) {
    case RADIO_STREAM_WATCHDOG_START_TIMEOUT:
        stream_failure_set(failure_message, failure_capacity,
                            "La emisora no inició a tiempo");
        return ESP_ERR_TIMEOUT;
    case RADIO_STREAM_WATCHDOG_PROGRESS_TIMEOUT:
        stream_failure_set(failure_message, failure_capacity,
                            "La emisora dejó de enviar audio");
        return ESP_ERR_TIMEOUT;
    case RADIO_STREAM_WATCHDOG_OK:
    default:
        return ESP_OK;
    }
}

static int stream_http_timeout_ms(const radio_stream_watchdog_t *watchdog,
                                  int maximum_ms)
{
    const int64_t remaining_us =
        radio_stream_watchdog_remaining_us(watchdog);
    if (remaining_us <= 0) return 1;
    const uint64_t remaining_ms =
        ((uint64_t)remaining_us + 999U) / 1000U;
    return remaining_ms < (uint64_t)maximum_ms
               ? (int)remaining_ms : maximum_ms;
}

static void stream_wait_retry_tick(const radio_stream_watchdog_t *watchdog)
{
    const int64_t tick_us = (int64_t)portTICK_PERIOD_MS * 1000LL;
    if (radio_stream_watchdog_remaining_us(watchdog) > tick_us) {
        vTaskDelay(1);
    }
}

static bool contains_case_insensitive(const char *text, const char *needle)
{
    if (text == NULL || needle == NULL || needle[0] == '\0') return false;
    for (const char *start = text; *start != '\0'; ++start) {
        const char *left = start;
        const char *right = needle;
        while (*left != '\0' && *right != '\0' &&
               tolower((unsigned char)*left) == tolower((unsigned char)*right)) {
            ++left;
            ++right;
        }
        if (*right == '\0') return true;
    }
    return false;
}

static bool stream_socket_error_is_transient(int socket_error)
{
    switch (socket_error) {
    case ETIMEDOUT:
    case ECONNRESET:
    case ECONNREFUSED:
    case ECONNABORTED:
    case ENETDOWN:
    case ENETUNREACH:
    case EHOSTUNREACH:
    case EPIPE:
        return true;
    default:
        return false;
    }
}

static esp_audio_simple_dec_type_t station_decoder_type(const radio_station_t *station)
{
    if (station == NULL || station->codec[0] == '\0' ||
        contains_case_insensitive(station->codec, "mp3") ||
        contains_case_insensitive(station->codec, "mpeg")) {
        /* Old saved favorites may not have codec metadata; preserve MP3 behavior. */
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
    if (contains_case_insensitive(station->codec, "aac")) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    }
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

static esp_err_t open_stream_response(esp_http_client_handle_t client,
                                      int *status_code,
                                      radio_stream_watchdog_t *watchdog,
                                      radio_stream_failure_reason_t *failure_reason,
                                      char *failure_message,
                                      size_t failure_capacity)
{
    for (int redirects = 0; redirects <= 6; ++redirects) {
        if (stop_requested()) {
            return ESP_ERR_INVALID_STATE;
        }
        esp_err_t watchdog_err = stream_watchdog_check(
            watchdog, failure_message, failure_capacity);
        if (watchdog_err != ESP_OK) return watchdog_err;
        (void)esp_http_client_set_timeout_ms(
            client, stream_http_timeout_ms(watchdog,
                                           PLAYER_HTTP_CONNECT_TIMEOUT_MS));
        esp_err_t err = RADIO_TEST_NETWORK_GATE();
        if (err == ESP_OK) err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            watchdog_err = stream_watchdog_check(
                watchdog, failure_message, failure_capacity);
            if (watchdog_err != ESP_OK) return watchdog_err;
            int tls_error = 0;
            int tls_flags = 0;
            const esp_err_t tls_status =
                esp_http_client_get_and_clear_last_tls_error(client,
                                                             &tls_error,
                                                             &tls_flags);
            const int socket_errno = esp_http_client_get_errno(client);
            if (failure_reason != NULL) {
                if (err == ESP_ERR_TIMEOUT ||
                    err == ESP_ERR_HTTP_CONNECTING ||
                    socket_errno == ETIMEDOUT) {
                    *failure_reason = RADIO_STREAM_FAILURE_TIMEOUT;
                } else if (tls_flags != 0 ||
                           (tls_status != ESP_OK && tls_error != 0)) {
                    *failure_reason = RADIO_STREAM_FAILURE_OTHER;
                } else if (err == ESP_ERR_HTTP_CONNECT ||
                           stream_socket_error_is_transient(socket_errno)) {
                    *failure_reason = RADIO_STREAM_FAILURE_CONNECTION;
                }
            }
            if (tls_status != ESP_OK || tls_error != 0 || tls_flags != 0) {
                ESP_LOGW(TAG,
                         "Fallo TLS al abrir emisora: %s, mbedTLS=0x%x, flags=0x%x, socket errno=%d",
                         esp_err_to_name(tls_status), (unsigned)tls_error,
                         (unsigned)tls_flags, socket_errno);
            } else {
                ESP_LOGW(TAG,
                         "Fallo de conexión al abrir emisora: %s, socket errno=%d",
                         esp_err_to_name(err), socket_errno);
            }
            if (tls_flags != 0) {
                stream_failure_set(failure_message, failure_capacity,
                                   "Certificado HTTPS no confiable");
            } else if (tls_status != ESP_OK || tls_error != 0) {
                if (tls_error != 0 && failure_message != NULL &&
                    failure_capacity > 0) {
                    const int64_t tls_magnitude = tls_error < 0
                                                      ? -(int64_t)tls_error
                                                      : tls_error;
                    snprintf(failure_message, failure_capacity,
                             "Fallo HTTPS (TLS 0x%04x)",
                             (unsigned)tls_magnitude);
                } else {
                    stream_failure_set(failure_message, failure_capacity,
                                       "Fallo al negociar HTTPS");
                }
            } else if (socket_errno == ETIMEDOUT) {
                stream_failure_set(failure_message, failure_capacity,
                                   "La emisora no responde");
            } else {
                stream_failure_set(failure_message, failure_capacity,
                                   "No se pudo conectar a la emisora");
            }
            return err;
        }

        /* Bound header waits so the stop flag and startup deadline are polled. */
        int64_t content_length;
        do {
            (void)esp_http_client_set_timeout_ms(
                client, stream_http_timeout_ms(watchdog,
                                              PLAYER_HTTP_READ_TIMEOUT_MS));
            content_length = esp_http_client_fetch_headers(client);
            if (content_length == -ESP_ERR_HTTP_EAGAIN && !stop_requested()) {
                watchdog_err = stream_watchdog_check(
                    watchdog, failure_message, failure_capacity);
                if (watchdog_err != ESP_OK) {
                    esp_http_client_close(client);
                    return watchdog_err;
                }
                stream_wait_retry_tick(watchdog);
            }
        } while (content_length == -ESP_ERR_HTTP_EAGAIN && !stop_requested());
        if (stop_requested()) {
            esp_http_client_close(client);
            return ESP_ERR_INVALID_STATE;
        }
        watchdog_err = stream_watchdog_check(
            watchdog, failure_message, failure_capacity);
        if (watchdog_err != ESP_OK) {
            esp_http_client_close(client);
            return watchdog_err;
        }
        if (content_length < 0) {
            esp_http_client_close(client);
            if (failure_reason != NULL) {
                *failure_reason = content_length == -ESP_ERR_HTTP_READ_TIMEOUT
                                      ? RADIO_STREAM_FAILURE_TIMEOUT
                                      : RADIO_STREAM_FAILURE_CONNECTION;
            }
            stream_failure_set(failure_message, failure_capacity,
                                "Respuesta HTTP no válida");
            return ESP_FAIL;
        }

        *status_code = esp_http_client_get_status_code(client);
        if (*status_code >= 200 && *status_code < 300) {
            return ESP_OK;
        }
        if (!is_redirect_status(*status_code) || redirects == 6) {
            esp_http_client_close(client);
            if (failure_reason != NULL) {
                *failure_reason = RADIO_STREAM_FAILURE_HTTP_STATUS;
            }
            if (failure_message != NULL && failure_capacity > 0) {
                snprintf(failure_message, failure_capacity,
                         "La emisora respondió con HTTP %d", *status_code);
            }
            return ESP_FAIL;
        }

        err = esp_http_client_set_redirection(client);
        esp_http_client_close(client);
        if (err != ESP_OK) {
            if (failure_reason != NULL) {
                *failure_reason = RADIO_STREAM_FAILURE_PROTOCOL;
            }
            stream_failure_set(failure_message, failure_capacity,
                               "Redirección no válida de la emisora");
            return err;
        }
    }
    if (failure_reason != NULL) {
        *failure_reason = RADIO_STREAM_FAILURE_PROTOCOL;
    }
    stream_failure_set(failure_message, failure_capacity,
                       "Demasiadas redirecciones");
    return ESP_FAIL;
}

static esp_err_t play_stream(const radio_station_t *station,
                             uint32_t generation,
                             radio_stream_watchdog_t *watchdog,
                             radio_stream_failure_reason_t *failure_reason,
                             int *http_status,
                             char *failure_message,
                             size_t failure_capacity)
{
    if (failure_message != NULL && failure_capacity > 0) {
        failure_message[0] = '\0';
    }
    if (failure_reason != NULL) {
        *failure_reason = RADIO_STREAM_FAILURE_OTHER;
    }
    if (http_status != NULL) *http_status = 0;
    if (!radio_wifi_is_connected()) {
        if (failure_reason != NULL) {
            *failure_reason = RADIO_STREAM_FAILURE_CONNECTION;
        }
        stream_failure_set(failure_message, failure_capacity,
                            "Sin conexión Wi-Fi");
        return ESP_ERR_INVALID_STATE;
    }

    if (station == NULL || !radio_stream_url_is_valid(station->url)) {
        if (failure_reason != NULL) {
            *failure_reason = RADIO_STREAM_FAILURE_INVALID_URL;
        }
        stream_failure_set(failure_message, failure_capacity,
                            "URL de emisora no válida");
        return ESP_ERR_INVALID_ARG;
    }
    if (watchdog == NULL) {
        stream_failure_set(failure_message, failure_capacity,
                           "No se pudo iniciar el reloj de la emisora");
        return ESP_ERR_INVALID_ARG;
    }

    const esp_audio_simple_dec_type_t decoder_type = station_decoder_type(station);
    if (decoder_type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
        if (failure_reason != NULL) {
            *failure_reason = RADIO_STREAM_FAILURE_UNSUPPORTED_FORMAT;
        }
        char unsupported_message[PLAYER_STATUS_MESSAGE_MAX];
        snprintf(unsupported_message, sizeof(unsupported_message),
                 "Formato %.24s no compatible (MP3/AAC)", station->codec);
        stream_failure_set(failure_message, failure_capacity, unsupported_message);
        ESP_LOGW(TAG, "Formato no compatible para %s: %s",
                 station->name, station->codec);
        return ESP_ERR_NOT_SUPPORTED;
    }
    const bool is_aac = decoder_type == ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    const char *format_name = is_aac ? "AAC" : "MP3";

    ESP_LOGI(TAG, "Conectando a %s", station->name);
    stream_context_t stream = {
        .pcm_stream = s_pcm_stream,
        .output_capacity = PLAYER_OUTPUT_SIZE,
        .generation = generation,
        .watchdog = watchdog,
        .format_name = format_name,
    };
    stream.output_buffer = RADIO_TEST_HEAP_CAPS_MALLOC(
        stream.output_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (stream.output_buffer == NULL) {
        if (failure_reason != NULL) {
            *failure_reason = RADIO_STREAM_FAILURE_RESOURCE;
        }
        stream.output_buffer = RADIO_TEST_MALLOC(stream.output_capacity);
    }
    if (stream.output_buffer == NULL) {
        stream_failure_set(failure_message, failure_capacity,
                           "Memoria insuficiente para reproducir");
        return ESP_ERR_NO_MEM;
    }
    stream.input_capacity = PLAYER_PENDING_MAX;
    stream.input_buffer = RADIO_TEST_HEAP_CAPS_MALLOC(
        stream.input_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (stream.input_buffer == NULL) {
        if (failure_reason != NULL) {
            *failure_reason = RADIO_STREAM_FAILURE_RESOURCE;
        }
        stream.input_buffer = RADIO_TEST_MALLOC(stream.input_capacity);
    }
    if (stream.input_buffer == NULL) {
        free(stream.output_buffer);
        stream_failure_set(failure_message, failure_capacity,
                           "Memoria insuficiente para reproducir");
        return ESP_ERR_NO_MEM;
    }

    esp_aac_dec_cfg_t aac_config = ESP_AAC_DEC_CONFIG_DEFAULT();
    /* AAC+ is common among internet radio streams; configure it only for AAC. */
    aac_config.aac_plus_enable = true;
    esp_audio_simple_dec_cfg_t decoder_config = {
        .dec_type = decoder_type,
        .dec_cfg = is_aac ? &aac_config : NULL,
        .cfg_size = is_aac ? (int)sizeof(aac_config) : 0,
        .use_frame_dec = false,
    };
    esp_audio_err_t dec_err = esp_audio_simple_dec_open(&decoder_config, &stream.decoder);
    if (dec_err != ESP_AUDIO_ERR_OK) {
        /* A failed allocation is a resource shortage, not an unsupported
           format; reporting it as a format error would tell the user to change
           codec when the real cause is memory. */
        const bool resource_error =
            radio_stream_decoder_error_is_resource((int)dec_err);
        if (failure_reason != NULL) {
            *failure_reason = resource_error
                                  ? RADIO_STREAM_FAILURE_RESOURCE
                                  : RADIO_STREAM_FAILURE_UNSUPPORTED_FORMAT;
        }
        free(stream.output_buffer);
        free(stream.input_buffer);
        stream_failure_set(failure_message, failure_capacity,
                           resource_error
                               ? "Memoria insuficiente para iniciar el audio"
                               : (is_aac
                                      ? "No se pudo iniciar el decodificador AAC"
                                      : "No se pudo iniciar el decodificador MP3"));
        return resource_error ? ESP_ERR_NO_MEM : ESP_FAIL;
    }

    stream.stop_requested = false;
    const esp_http_client_config_t http_config = {
        .url = station->url,
        /* Stream hosts often need several seconds for DNS and TLS setup. */
        .timeout_ms = PLAYER_HTTP_CONNECT_TIMEOUT_MS,
        .buffer_size = PLAYER_INPUT_SIZE,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .max_redirection_count = 6,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (client == NULL) {
        if (failure_reason != NULL) {
            *failure_reason = RADIO_STREAM_FAILURE_RESOURCE;
        }
        esp_audio_simple_dec_close(stream.decoder);
        free(stream.output_buffer);
        free(stream.input_buffer);
        stream_failure_set(failure_message, failure_capacity,
                           "Memoria insuficiente para conectar");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = esp_http_client_set_header(
        client, "User-Agent", "OndaPortatil/0.1 ESP-IDF");
    if (result == ESP_OK) {
        result = esp_http_client_set_header(client, "Icy-MetaData", "0");
    }
    if (result == ESP_OK) {
        result = esp_http_client_set_header(
            client, "Accept", "audio/mpeg, audio/*;q=0.8, */*;q=0.5");
    }
    int status_code = 0;
    if (result == ESP_OK) {
        result = open_stream_response(client, &status_code, stream.watchdog,
                                      failure_reason, failure_message,
                                      failure_capacity);
    } else {
        if (failure_reason != NULL && result == ESP_ERR_NO_MEM) {
            *failure_reason = RADIO_STREAM_FAILURE_RESOURCE;
        }
        stream_failure_set(failure_message, failure_capacity,
                           result == ESP_ERR_NO_MEM
                               ? "Memoria insuficiente para conectar"
                                : "No se pudo preparar la conexión");
    }
    if (http_status != NULL) *http_status = status_code;
    if (result == ESP_ERR_TIMEOUT && failure_reason != NULL &&
        *failure_reason == RADIO_STREAM_FAILURE_OTHER) {
        *failure_reason = RADIO_STREAM_FAILURE_TIMEOUT;
    }
    if (result != ESP_OK && !stop_requested()) {
        if (status_code != 0) {
            ESP_LOGW(TAG, "La emisora %s respondió con HTTP %d",
                     station->name, status_code);
        } else {
            ESP_LOGW(TAG, "No se pudo establecer el stream de %s: %s",
                     station->name, esp_err_to_name(result));
        }
    }
    if (result == ESP_OK) {
        /* Short reads keep stop and station switching responsive; EAGAIN is retried. */
        esp_http_client_set_timeout_ms(client, PLAYER_HTTP_READ_TIMEOUT_MS);
    }
    /* A chunked response is an open-ended live stream: its end is a dropped
       connection, not a normal completion. A Content-Length response ending is
       a legitimate end of stream. */
    const bool live_stream =
        result == ESP_OK && esp_http_client_is_chunked_response(client);
    uint8_t *input = NULL;
    if (result == ESP_OK) {
        input = RADIO_TEST_HEAP_CAPS_MALLOC(
            PLAYER_INPUT_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (input == NULL) input = RADIO_TEST_MALLOC(PLAYER_INPUT_SIZE);
        if (input == NULL) {
            result = ESP_ERR_NO_MEM;
            if (failure_reason != NULL) {
                *failure_reason = RADIO_STREAM_FAILURE_RESOURCE;
            }
            stream_failure_set(failure_message, failure_capacity,
                               "Memoria insuficiente para conectar");
        }
    }
    while (result == ESP_OK && !stop_requested()) {
        result = stream_watchdog_check(stream.watchdog, failure_message,
                                       failure_capacity);
        if (result != ESP_OK) break;
        esp_http_client_set_timeout_ms(
            client, stream_http_timeout_ms(stream.watchdog,
                                           PLAYER_HTTP_READ_TIMEOUT_MS));
        const int64_t read_started_us = esp_timer_get_time();
        const int bytes_read = esp_http_client_read(client, (char *)input,
                                                    PLAYER_INPUT_SIZE);
        const int64_t read_duration_us = esp_timer_get_time() - read_started_us;
        if (read_duration_us > 0 && (uint64_t)read_duration_us > stream.http_read_max_us) {
            stream.http_read_max_us = read_duration_us > UINT32_MAX
                                          ? UINT32_MAX
                                          : (uint32_t)read_duration_us;
        }
        if (stop_requested()) {
            stream.stop_requested = true;
            break;
        }
        if (bytes_read == -ESP_ERR_HTTP_EAGAIN) {
            increment_u32_saturating(&stream.http_read_eagain);
            result = stream_watchdog_check(stream.watchdog, failure_message,
                                           failure_capacity);
            if (result != ESP_OK) break;
            stream_wait_retry_tick(stream.watchdog);
            audio_metrics_log(&stream, false);
            continue;
        }
        result = stream_watchdog_check(stream.watchdog, failure_message,
                                       failure_capacity);
        if (result != ESP_OK) break;
        if (bytes_read <= 0) {
            if (bytes_read < 0) {
                if (bytes_read == -ESP_ERR_HTTP_READ_TIMEOUT) {
                    result = ESP_ERR_TIMEOUT;
                    if (failure_reason != NULL) {
                        *failure_reason = RADIO_STREAM_FAILURE_TIMEOUT;
                    }
                    stream_failure_set(failure_message, failure_capacity,
                                       "La emisora no responde");
                } else {
                    result = ESP_FAIL;
                    if (failure_reason != NULL) {
                        *failure_reason = RADIO_STREAM_FAILURE_CONNECTION;
                    }
                    stream_failure_set(failure_message, failure_capacity,
                                        "Se interrumpió la transmisión");
                }
            } else if (radio_stream_eof_is_interruption(
                           live_stream,
                           esp_http_client_is_complete_data_received(client))) {
                /* Live EOF and a truncated finite response are interruptions.
                   A fully received Content-Length response ends normally. */
                result = ESP_FAIL;
                if (failure_reason != NULL) {
                    *failure_reason = RADIO_STREAM_FAILURE_CONNECTION;
                }
                stream_failure_set(failure_message, failure_capacity,
                                    "La emisora cerró la transmisión");
            }
            break;
        }
        const int64_t decode_started_us = esp_timer_get_time();
        result = decode_chunk(&stream, input, (size_t)bytes_read);
        const int64_t decode_duration_us = esp_timer_get_time() - decode_started_us;
        if (decode_duration_us > 0 &&
            (uint64_t)decode_duration_us > stream.decode_max_us) {
            stream.decode_max_us = decode_duration_us > UINT32_MAX
                                        ? UINT32_MAX
                                        : (uint32_t)decode_duration_us;
        }
        if (result == ESP_ERR_TIMEOUT) {
            if (failure_reason != NULL &&
                *failure_reason == RADIO_STREAM_FAILURE_OTHER) {
                *failure_reason = RADIO_STREAM_FAILURE_TIMEOUT;
            }
            (void)stream_watchdog_check(stream.watchdog, failure_message,
                                        failure_capacity);
        } else if (result == ESP_OK) {
            result = stream_watchdog_check(stream.watchdog, failure_message,
                                          failure_capacity);
            if (result == ESP_ERR_TIMEOUT && failure_reason != NULL) {
                *failure_reason = RADIO_STREAM_FAILURE_TIMEOUT;
            }
        } else if (failure_reason != NULL) {
            if (result == ESP_ERR_NOT_SUPPORTED) {
                *failure_reason = RADIO_STREAM_FAILURE_UNSUPPORTED_FORMAT;
            } else if (result == ESP_ERR_NO_MEM) {
                *failure_reason = RADIO_STREAM_FAILURE_RESOURCE;
            } else if (result == ESP_ERR_INVALID_SIZE ||
                       result == ESP_ERR_INVALID_RESPONSE) {
                *failure_reason = RADIO_STREAM_FAILURE_PROTOCOL;
            }
        }
        audio_metrics_log(&stream, false);
    }
    if (stop_requested()) {
        stream.stop_requested = true;
    }
    if (result != ESP_OK || stream.stop_requested) {
        /* Do not drain stale PCM after a failed or superseded session. */
        stream_abort_sink(&stream);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(input);

    if (stream.audio_sink_started) {
        stream_finish_producer(&stream);
        if (stream.stop_requested || stop_requested()) {
            radio_board_audio_mute();
        }
        (void)xSemaphoreTake(s_audio_sink_done, portMAX_DELAY);
        stream.audio_sink_started = false;
        const esp_err_t sink_err = stream_sink_error(&stream);
        if (result == ESP_OK && sink_err != ESP_OK) {
            result = sink_err;
            if (failure_reason != NULL) {
                *failure_reason = RADIO_STREAM_FAILURE_OUTPUT;
            }
        }
        audio_metrics_log(&stream, true);
    }

    if (stream.audio_open) {
        const esp_err_t close_err = radio_board_audio_close();
        if (close_err != ESP_OK) {
            ESP_LOGW(TAG, "No se pudo cerrar la salida de audio: %s",
                     esp_err_to_name(close_err));
        }
    }
    esp_audio_simple_dec_close(stream.decoder);
    free(stream.output_buffer);
    free(stream.input_buffer);

    if (stream.stop_requested || stop_requested()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (result != ESP_OK) {
        if (result == ESP_ERR_TIMEOUT && failure_reason != NULL) {
            *failure_reason = RADIO_STREAM_FAILURE_TIMEOUT;
        }
        ESP_LOGW(TAG, "Stream finalizo: %s", esp_err_to_name(result));
        if (failure_message != NULL && failure_capacity > 0 &&
            failure_message[0] == '\0') {
            stream_failure_set(failure_message, failure_capacity,
                               result == ESP_ERR_NO_MEM
                                   ? "Memoria insuficiente para reproducir"
                                    : "Error en la transmisión");
        }
        return result;
    }
    if (!stream.saw_audio) {
        if (failure_reason != NULL) {
            *failure_reason = stream.decode_error
                                  ? RADIO_STREAM_FAILURE_UNSUPPORTED_FORMAT
                                  : RADIO_STREAM_FAILURE_CONNECTION;
        }
        const esp_err_t no_audio_result = stream.decode_error
                                              ? ESP_ERR_NOT_SUPPORTED
                                              : ESP_ERR_INVALID_RESPONSE;
        stream_failure_set(failure_message, failure_capacity,
                           stream.decode_error
                                ? "Formato no compatible (MP3/AAC)"
                                : "La emisora no envió audio");
        return no_audio_result;
    }
    return ESP_OK;
}

static bool retry_backoff_wait(uint32_t generation, uint32_t delay_ms,
                               const radio_stream_watchdog_t *watchdog,
                               bool *deadline_expired)
{
    if (deadline_expired != NULL) *deadline_expired = false;
    TickType_t remaining = pdMS_TO_TICKS(delay_ms);
    if (remaining == 0U) remaining = 1U;
    TickType_t slice = pdMS_TO_TICKS(50U);
    if (slice == 0U) slice = 1U;

    while (remaining > 0U) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool cancelled = s_stop_requested ||
                               generation != s_play_generation ||
                               s_has_pending_station;
        xSemaphoreGive(s_lock);
        if (cancelled) return false;

        if (radio_stream_watchdog_check(watchdog) !=
            RADIO_STREAM_WATCHDOG_OK) {
            if (deadline_expired != NULL) *deadline_expired = true;
            return false;
        }
        const int64_t remaining_us =
            radio_stream_watchdog_remaining_us(watchdog);
        const uint64_t remaining_ms =
            ((uint64_t)remaining_us + 999U) / 1000U;
        TickType_t deadline_ticks =
            pdMS_TO_TICKS((uint32_t)remaining_ms);
        if (remaining_us <= 0 || deadline_ticks == 0U) {
            if (deadline_expired != NULL) *deadline_expired = true;
            return false;
        }
        TickType_t wait_ticks = remaining < slice ? remaining : slice;
        if (deadline_ticks < wait_ticks) wait_ticks = deadline_ticks;
        vTaskDelay(wait_ticks);
        remaining -= wait_ticks;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool current = !s_stop_requested && generation == s_play_generation &&
                         !s_has_pending_station;
    xSemaphoreGive(s_lock);
    if (!current) return false;
    if (radio_stream_watchdog_check(watchdog) != RADIO_STREAM_WATCHDOG_OK) {
        if (deadline_expired != NULL) *deadline_expired = true;
        return false;
    }
    return true;
}

static void player_worker(void *arg)
{
    (void)arg;
    const uint32_t supervisor_registration =
        radio_task_supervisor_register(RADIO_SUPERVISED_TASK_AUDIO_PLAYER);
    for (;;) {
        radio_task_supervisor_progress(
            RADIO_SUPERVISED_TASK_AUDIO_PLAYER);
        radio_station_t station;
        player_status_event_t event = {0};
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (!s_has_pending_station) {
            s_worker = NULL;
            s_stop_requested = false;
            status_event_update_locked(&event, RADIO_PLAYER_STATE_IDLE, "Detenido");
            xSemaphoreGive(s_lock);
            status_event_dispatch(&event);
            radio_task_supervisor_unregister(
                RADIO_SUPERVISED_TASK_AUDIO_PLAYER, supervisor_registration);
            vTaskDelete(NULL);
            return;
        }
        station = s_pending_station;
        s_current_station = station;
        s_has_pending_station = false;
        s_stop_requested = false;
        const uint32_t generation = s_play_generation;
        xSemaphoreGive(s_lock);

        char failure_message[PLAYER_STATUS_MESSAGE_MAX] = {0};
        radio_stream_failure_reason_t failure_reason =
            RADIO_STREAM_FAILURE_OTHER;
        int http_status = 0;
        uint8_t retry_index = 0;
        radio_stream_watchdog_t session_watchdog;
        esp_err_t result = ESP_OK;
        if (!radio_stream_watchdog_init(&session_watchdog,
                                        stream_monotonic_clock, NULL)) {
            result = ESP_ERR_INVALID_STATE;
            stream_failure_set(failure_message, sizeof(failure_message),
                               "No se pudo iniciar el reloj de la emisora");
        }
        if (result == ESP_OK) {
            for (;;) {
                failure_reason = RADIO_STREAM_FAILURE_OTHER;
                http_status = 0;
                result = play_stream(&station, generation, &session_watchdog,
                                     &failure_reason, &http_status,
                                     failure_message, sizeof(failure_message));
                if (result == ESP_OK || stop_requested()) {
                    break;
                }

                const uint32_t retry_delay =
                    radio_stream_retry_delay_ms(retry_index);
                if (retry_delay == 0U ||
                    !radio_stream_failure_is_retryable(failure_reason,
                                                       http_status)) {
                    break;
                }

                char retry_message[PLAYER_STATUS_MESSAGE_MAX];
                snprintf(retry_message, sizeof(retry_message),
                          "Reconectando (%u/%u)…",
                         (unsigned)(retry_index + 1U),
                         (unsigned)RADIO_STREAM_MAX_RETRIES);
                notify_status_for_generation(generation,
                                             RADIO_PLAYER_STATE_RECOVERING,
                                             retry_message);
                bool deadline_expired = false;
                if (!retry_backoff_wait(generation, retry_delay,
                                        &session_watchdog,
                                        &deadline_expired)) {
                    if (deadline_expired) {
                        result = stream_watchdog_check(
                            &session_watchdog, failure_message,
                            sizeof(failure_message));
                        if (result == ESP_OK) {
                            /* A sub-tick remainder cannot safely contain the
                               next backoff even if the clock has not crossed
                               the exact deadline yet. Never report success
                               when abandoning the retry loop for that reason. */
                            result = ESP_ERR_TIMEOUT;
                            stream_failure_set(
                                failure_message, sizeof(failure_message),
                                session_watchdog.has_progress
                                     ? "La emisora dejó de enviar audio"
                                     : "La emisora no inició a tiempo");
                        }
                        failure_reason = RADIO_STREAM_FAILURE_TIMEOUT;
                    } else {
                        result = ESP_ERR_INVALID_STATE;
                    }
                    break;
                }
                ++retry_index;
            }
        }
         ESP_LOGI(TAG, "Reserva mínima de stack del reproductor: %u bytes",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "No se pudo reproducir %s: %s", station.name,
                     esp_err_to_name(result));
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool has_next = s_has_pending_station;
        if (!has_next) {
            const bool result_is_current = generation == s_play_generation;
            const bool pause_completed = s_pause_requested && result_is_current;
            s_worker = NULL;
            s_stop_requested = false;
            s_pause_requested = false;
            if (pause_completed) {
                status_event_update_locked(
                    &event, RADIO_PLAYER_STATE_PAUSED,
                    "Pausa · vuelve en vivo");
            } else if (!result_is_current) {
                /* Preserve a newer terminal ERROR; STOPPING still settles at IDLE. */
                if (s_state == RADIO_PLAYER_STATE_STOPPING) {
                    status_event_update_locked(&event, RADIO_PLAYER_STATE_IDLE,
                                               "Detenido");
                }
            } else if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
                status_event_update_locked(
                    &event, RADIO_PLAYER_STATE_ERROR,
                    failure_message[0] != '\0'
                        ? failure_message
                        : (result == ESP_ERR_NOT_SUPPORTED
                                ? "Formato no compatible (MP3/AAC)"
                               : "No se pudo abrir la emisora"));
            } else {
                status_event_update_locked(
                    &event, RADIO_PLAYER_STATE_IDLE,
                    result == ESP_OK ? "Fin de la transmisión" : "Detenido");
            }
            xSemaphoreGive(s_lock);
            status_event_dispatch(&event);
            radio_task_supervisor_unregister(
                RADIO_SUPERVISED_TASK_AUDIO_PLAYER, supervisor_registration);
            vTaskDelete(NULL);
            return;
        }
        xSemaphoreGive(s_lock);
    }
}

static esp_err_t pcm_buffer_init(void)
{
    if (s_pcm_stream != NULL) return ESP_OK;

    const size_t candidates[] = {
        PLAYER_PCM_BUFFER_SIZE,
        128U * 1024U,
        PLAYER_PCM_MIN_BUFFER_SIZE,
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        s_pcm_storage = RADIO_TEST_HEAP_CAPS_MALLOC(
            candidates[i], MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_pcm_storage != NULL) {
            s_pcm_capacity = candidates[i];
            break;
        }
    }
    if (s_pcm_storage == NULL) {
        ESP_LOGE(TAG, "No hay PSRAM para la cola PCM (%u KiB mínimos)",
                 (unsigned)(PLAYER_PCM_MIN_BUFFER_SIZE / 1024U));
        return ESP_ERR_NO_MEM;
    }

    s_pcm_stream = xStreamBufferCreateStatic(s_pcm_capacity, 1,
                                             s_pcm_storage,
                                             &s_pcm_stream_state);
    if (s_pcm_stream == NULL) {
        heap_caps_free(s_pcm_storage);
        s_pcm_storage = NULL;
        s_pcm_capacity = 0;
        return ESP_ERR_NO_MEM;
    }
    s_audio_sink_done = xSemaphoreCreateBinary();
    if (s_audio_sink_done == NULL) {
        vStreamBufferDelete(s_pcm_stream);
        s_pcm_stream = NULL;
        heap_caps_free(s_pcm_storage);
        s_pcm_storage = NULL;
        s_pcm_capacity = 0;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Cola PCM fija: %u KiB en PSRAM; precarga objetivo %u KiB",
             (unsigned)(s_pcm_capacity / 1024U),
             (unsigned)((s_pcm_capacity / 4U) / 1024U));
    return ESP_OK;
}

esp_err_t radio_player_init(void)
{
    if (s_initialized) return ESP_OK;
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_volume_queue == NULL) {
        s_volume_queue = xQueueCreate(1, sizeof(uint8_t));
        if (s_volume_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_status_queue == NULL) {
        s_status_queue = xQueueCreate(1, sizeof(player_status_event_t));
        if (s_status_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_volume_worker == NULL &&
        xTaskCreate(volume_worker, "radio_volume", 3072, NULL, 4,
                    &s_volume_worker) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(radio_board_audio_init(), TAG, "audio init failed");
    ESP_RETURN_ON_ERROR(pcm_buffer_init(), TAG, "PCM buffer init failed");
    if (!s_decoder_initialized) {
        if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK ||
            esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) {
            return ESP_FAIL;
        }
        s_decoder_initialized = true;
    }
    s_volume = radio_station_store_volume();
    const esp_err_t volume_err = radio_board_audio_set_volume(s_volume);
    if (volume_err != ESP_OK) return volume_err;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_initialized = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void radio_player_restore_station(const radio_station_t *station)
{
    if (station == NULL || station->url[0] == '\0' || s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_worker == NULL) s_current_station = *station;
    xSemaphoreGive(s_lock);
}

esp_err_t radio_player_play(const radio_station_t *station)
{
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    if (station == NULL || station->url[0] == '\0' ||
        station->name[0] == '\0' ||
        memchr(station->url, '\0', sizeof(station->url)) == NULL ||
        memchr(station->name, '\0', sizeof(station->name)) == NULL ||
        memchr(station->codec, '\0', sizeof(station->codec)) == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool initialized = s_initialized;
    xSemaphoreGive(s_lock);
    if (!initialized) return ESP_ERR_INVALID_STATE;
    if (!radio_wifi_is_connected()) {
        player_status_event_t offline_event = {0};
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool active = s_worker != NULL;
        s_current_station = *station;
        s_has_pending_station = false;
        s_stop_requested = active;
        s_pause_requested = false;
        s_play_generation = next_generation(s_play_generation);
        status_event_update_locked(&offline_event, RADIO_PLAYER_STATE_ERROR,
                                   "Sin conexión Wi-Fi");
        xSemaphoreGive(s_lock);
        radio_board_audio_mute();
        status_event_dispatch(&offline_event);
        return ESP_ERR_INVALID_STATE;
    }

    player_status_event_t event = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool replacing_stream = s_worker != NULL;
    s_pending_station = *station;
    s_current_station = *station;
    s_has_pending_station = true;
    s_stop_requested = true;
    s_pause_requested = false;
    s_play_generation = next_generation(s_play_generation);
    if (s_worker == NULL) {
        BaseType_t created = xTaskCreate(player_worker, "radio_player", 10240,
                                         NULL, 5, &s_worker);
        if (created != pdPASS) {
            created = xTaskCreate(player_worker, "radio_player", 8192,
                                  NULL, 5, &s_worker);
            if (created == pdPASS) {
                ESP_LOGW(TAG, "Reproductor iniciado con stack de 8 KB");
            }
        }
        if (created != pdPASS) {
            ESP_LOGE(TAG, "Sin memoria para tarea: interna libre %u, bloque mayor %u, PSRAM libre %u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            s_has_pending_station = false;
            s_stop_requested = false;
            status_event_update_locked(&event, RADIO_PLAYER_STATE_ERROR,
                                "No hay memoria para iniciar la reproducción");
            xSemaphoreGive(s_lock);
            status_event_dispatch(&event);
            return ESP_ERR_NO_MEM;
        }
    }
    status_event_update_locked(&event, RADIO_PLAYER_STATE_CONNECTING,
                                replacing_stream ? "Cambiando de emisora…" : "Conectando…");
    xSemaphoreGive(s_lock);
    if (replacing_stream) {
        radio_board_audio_mute();
    }
    status_event_dispatch(&event);
    return ESP_OK;
}

void radio_player_stop(void)
{
    if (s_lock == NULL) {
        return;
    }
    player_status_event_t event = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool active = s_worker != NULL;
    s_has_pending_station = false;
    s_stop_requested = active;
    s_pause_requested = false;
    s_play_generation = next_generation(s_play_generation);
    status_event_update_locked(&event,
                               active ? RADIO_PLAYER_STATE_STOPPING
                                      : RADIO_PLAYER_STATE_IDLE,
                                active ? "Deteniendo…" : "Detenido");
    xSemaphoreGive(s_lock);
    radio_board_audio_mute();
    status_event_dispatch(&event);
}

void radio_player_toggle(void)
{
    radio_player_pause_toggle();
}

void radio_player_pause_toggle(void)
{
    if (s_lock == NULL) {
        return;
    }
    player_status_event_t event = {0};
    bool should_pause = false;
    bool should_resume = false;
    radio_station_t station = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state == RADIO_PLAYER_STATE_PAUSED && s_current_station.url[0] != '\0') {
        station = s_current_station;
        should_resume = true;
    } else if (s_worker != NULL &&
               (s_state == RADIO_PLAYER_STATE_CONNECTING ||
                s_state == RADIO_PLAYER_STATE_RECOVERING ||
                s_state == RADIO_PLAYER_STATE_PLAYING)) {
        s_pause_requested = true;
        s_stop_requested = true;
        s_has_pending_station = false;
        should_pause = true;
        status_event_update_locked(
            &event, RADIO_PLAYER_STATE_PAUSED,
            "Pausa · vuelve en vivo");
    } else if (s_current_station.url[0] != '\0') {
        station = s_current_station;
        should_resume = true;
    }
    xSemaphoreGive(s_lock);
    if (should_pause) {
        radio_board_audio_mute();
        status_event_dispatch(&event);
    } else if (should_resume) {
        radio_player_play(&station);
    }
}

int radio_player_volume(void)
{
    if (s_lock == NULL) {
        return s_volume;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int volume = s_volume;
    xSemaphoreGive(s_lock);
    return volume;
}

static void volume_set(int volume, bool persist)
{
    if (s_lock == NULL || s_volume_queue == NULL) return;
    const int bounded = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool changed = bounded != s_volume;
    s_volume = bounded;
    xSemaphoreGive(s_lock);
    if (changed || persist) volume_schedule_apply(persist);
}

void radio_player_volume_preview(int volume)
{
    volume_set(volume, false);
}

void radio_player_volume_commit(void)
{
    if (s_lock != NULL && s_volume_queue != NULL) volume_schedule_apply(true);
}

void radio_player_volume_step(int delta)
{
    if (s_lock == NULL || s_volume_queue == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int next = delta > 100 ? 100 : (delta < -100 ? 0 : s_volume + delta);
    const int bounded = next < 0 ? 0 : (next > 100 ? 100 : next);
    const bool changed = bounded != s_volume;
    s_volume = bounded;
    xSemaphoreGive(s_lock);
    if (changed) volume_schedule_apply(true);
}

void radio_player_set_status_callback(radio_player_status_cb_t callback)
{
    if (s_lock == NULL) {
        return;
    }
    player_status_event_t event = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status_callback = callback;
    event.callback = callback;
    event.state = s_state;
    event.sequence = s_status_sequence;
    snprintf(event.station, sizeof(event.station), "%s", s_current_station.name);
    snprintf(event.message, sizeof(event.message), "%s", s_status_message);
    xSemaphoreGive(s_lock);
    status_event_dispatch(&event);
}
