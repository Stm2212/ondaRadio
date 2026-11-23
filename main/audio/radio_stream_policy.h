#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    RADIO_STREAM_FAILURE_OTHER = 0,
    RADIO_STREAM_FAILURE_TIMEOUT,
    RADIO_STREAM_FAILURE_CONNECTION,
    RADIO_STREAM_FAILURE_HTTP_STATUS,
    RADIO_STREAM_FAILURE_INVALID_URL,
    RADIO_STREAM_FAILURE_UNSUPPORTED_FORMAT,
    RADIO_STREAM_FAILURE_RESOURCE,
    RADIO_STREAM_FAILURE_PROTOCOL,
    RADIO_STREAM_FAILURE_OUTPUT,
} radio_stream_failure_reason_t;

#define RADIO_STREAM_MAX_RETRIES 2U

/* Audio decoder error codes (mirror of esp_audio_err_t) that mean a resource
   shortage (allocation failed or decoder buffer too small) rather than an
   unsupported format. Kept numeric so this policy module and its host tests do
   not depend on the codec component. */
#define RADIO_STREAM_DECODER_ERR_MEM_LACK (-2)
#define RADIO_STREAM_DECODER_ERR_BUFF_NOT_ENOUGH (-8)

bool radio_stream_decoder_error_is_resource(int decoder_error);

bool radio_stream_eof_is_interruption(bool live_stream, bool response_complete);
bool radio_stream_failure_is_retryable(radio_stream_failure_reason_t reason,
                                       int http_status);
uint32_t radio_stream_retry_delay_ms(uint8_t retry_index);
bool radio_stream_url_is_valid(const char *url);
