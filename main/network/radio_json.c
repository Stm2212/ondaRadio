#include "radio_json.h"

#include <stdbool.h>

static bool json_whitespace(char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

cJSON *radio_json_parse_exact(const char *json, size_t length)
{
    if (json == NULL || length == 0U) return NULL;

    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length, &parse_end, 0);
    if (root == NULL || parse_end == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    const char *const end = json + length;
    while (parse_end < end && json_whitespace(*parse_end)) ++parse_end;
    if (parse_end != end) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}
