#include "radio_wifi_form.h"

#include <stdlib.h>
#include <string.h>

#include "esp_wifi.h"

#include "radio_url_codec.h"

#define FORM_KEY_MAX 16

static bool decode_value(char *dst, size_t dst_size, const char *src)
{
    return radio_url_form_decode(dst, dst_size, src, strlen(src));
}

bool radio_wifi_form_parse(char *body, size_t length, radio_wifi_form_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (body == NULL || out == NULL || length == 0U) {
        return false;
    }
    if (memchr(body, '\0', length) != NULL) {
        return false;
    }
    out->authmode = RADIO_WIFI_FORM_AUTHMODE_AUTO;

    bool ssid_seen = false;
    bool password_seen = false;
    bool authmode_seen = false;
    bool malformed = false;
    char *saveptr = NULL;
    for (char *field = strtok_r(body, "&", &saveptr); field != NULL;
         field = strtok_r(NULL, "&", &saveptr)) {
        char *value = strchr(field, '=');
        if (value == NULL) {
            malformed = true;
            break;
        }
        *value++ = '\0';
        char key[FORM_KEY_MAX] = {0};
        if (!radio_url_form_decode(key, sizeof(key), field, strlen(field))) {
            malformed = true;
            break;
        }
        if (strcmp(key, "ssid") == 0) {
            if (ssid_seen ||
                !decode_value(out->ssid, sizeof(out->ssid), value)) {
                malformed = true;
                break;
            }
            ssid_seen = true;
        } else if (strcmp(key, "password") == 0) {
            if (password_seen ||
                !decode_value(out->password, sizeof(out->password), value)) {
                malformed = true;
                break;
            }
            password_seen = true;
        } else if (strcmp(key, "authmode") == 0) {
            if (authmode_seen) {
                malformed = true;
                break;
            }
            authmode_seen = true;
            char decoded[8] = {0};
            if (!decode_value(decoded, sizeof(decoded), value)) {
                malformed = true;
                break;
            }
            char *end = NULL;
            const long parsed = strtol(decoded, &end, 10);
            const bool is_auto = parsed == RADIO_WIFI_FORM_AUTHMODE_AUTO;
            const bool is_explicit = parsed >= 0 && parsed < WIFI_AUTH_MAX;
            if (end != decoded && *end == '\0' && (is_auto || is_explicit)) {
                out->authmode = (int)parsed;
            } else {
                malformed = true;
            }
        }
    }

    if (malformed || !ssid_seen) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
}
