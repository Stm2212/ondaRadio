#include "radio_stream_policy.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

bool radio_stream_decoder_error_is_resource(int decoder_error)
{
    return decoder_error == RADIO_STREAM_DECODER_ERR_MEM_LACK ||
           decoder_error == RADIO_STREAM_DECODER_ERR_BUFF_NOT_ENOUGH;
}

bool radio_stream_eof_is_interruption(bool live_stream, bool response_complete)
{
    return live_stream || !response_complete;
}

bool radio_stream_failure_is_retryable(radio_stream_failure_reason_t reason,
                                       int http_status)
{
    switch (reason) {
    case RADIO_STREAM_FAILURE_TIMEOUT:
    case RADIO_STREAM_FAILURE_CONNECTION:
        return true;
    case RADIO_STREAM_FAILURE_HTTP_STATUS:
        return http_status >= 500 && http_status <= 599;
    case RADIO_STREAM_FAILURE_OTHER:
    case RADIO_STREAM_FAILURE_INVALID_URL:
    case RADIO_STREAM_FAILURE_UNSUPPORTED_FORMAT:
    case RADIO_STREAM_FAILURE_RESOURCE:
    case RADIO_STREAM_FAILURE_PROTOCOL:
    case RADIO_STREAM_FAILURE_OUTPUT:
    default:
        return false;
    }
}

uint32_t radio_stream_retry_delay_ms(uint8_t retry_index)
{
    switch (retry_index) {
    case 0: return 1000U;
    case 1: return 2000U;
    default: return 0U;
    }
}

bool radio_stream_url_is_valid(const char *url)
{
    if (url == NULL || url[0] == '\0') return false;
    const char *separator = strstr(url, "://");
    if (separator == NULL) return false;
    const size_t scheme_length = (size_t)(separator - url);
    const bool http = scheme_length == 4U &&
                      tolower((unsigned char)url[0]) == 'h' &&
                      tolower((unsigned char)url[1]) == 't' &&
                      tolower((unsigned char)url[2]) == 't' &&
                      tolower((unsigned char)url[3]) == 'p';
    const bool https = scheme_length == 5U &&
                       tolower((unsigned char)url[0]) == 'h' &&
                       tolower((unsigned char)url[1]) == 't' &&
                       tolower((unsigned char)url[2]) == 't' &&
                       tolower((unsigned char)url[3]) == 'p' &&
                       tolower((unsigned char)url[4]) == 's';
    if (!http && !https) return false;

    const char *authority = separator + 3;
    const char *authority_end = strpbrk(authority, "/?#");
    if (authority_end == NULL) authority_end = url + strlen(url);
    if (authority == authority_end) return false;
    for (const char *cursor = authority; cursor < authority_end; ++cursor) {
        const unsigned char ch = (unsigned char)*cursor;
        if (ch <= 0x20U || ch == 0x7fU) return false;
    }

    const char *host = authority;
    for (const char *cursor = authority; cursor < authority_end; ++cursor) {
        if (*cursor == '@') host = cursor + 1;
    }
    if (host >= authority_end) return false;

    const char *port = NULL;
    const char *host_end = authority_end;
    if (*host == '[') {
        const char *closing = memchr(host, ']',
                                     (size_t)(authority_end - host));
        if (closing == NULL || closing == host + 1) return false;
        host_end = closing + 1;
        if (closing + 1 < authority_end) {
            if (closing[1] != ':') return false;
            port = closing + 2;
        }
    } else {
        const char *colon = memchr(host, ':',
                                   (size_t)(authority_end - host));
        if (colon != NULL) {
            if (memchr(colon + 1, ':',
                       (size_t)(authority_end - colon - 1)) != NULL) {
                return false;
            }
            host_end = colon;
            port = colon + 1;
        }
    }
    if (host_end <= host) return false;
    if (port != NULL) {
        if (port >= authority_end) return false;
        unsigned long port_number = 0;
        size_t digits = 0;
        for (const char *cursor = port; cursor < authority_end &&
                                        *cursor >= '0' && *cursor <= '9';
             ++cursor) {
            if (++digits > 5U) return false;
            port_number = port_number * 10U + (unsigned long)(*cursor - '0');
        }
        const char *port_end = port;
        while (port_end < authority_end &&
               *port_end >= '0' && *port_end <= '9') ++port_end;
        if (digits == 0U || port_end != authority_end || port_number == 0U ||
            port_number > 65535U) {
            return false;
        }
    }
    return true;
}
