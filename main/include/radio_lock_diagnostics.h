#pragma once

#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define RADIO_LOCK_WARN_WAIT_US 20000LL

/*
 * Measure mutex wait time without changing the caller's timeout or lock order.
 * This is diagnostic instrumentation, not a replacement for short critical
 * sections and service-lock release before callbacks/I/O.
 */
static inline BaseType_t radio_mutex_take_audited(
    SemaphoreHandle_t mutex, TickType_t timeout, const char *tag,
    const char *lock_name, const char *caller)
{
    const int64_t start_us = esp_timer_get_time();
    const BaseType_t acquired = xSemaphoreTake(mutex, timeout);
    const int64_t wait_us = esp_timer_get_time() - start_us;
    if (wait_us >= RADIO_LOCK_WARN_WAIT_US) {
        ESP_LOGW(tag, "Mutex %s waited %lld us at %s%s", lock_name,
                 (long long)wait_us, caller,
                 acquired == pdTRUE ? "" : " (timeout)");
    }
    return acquired;
}
